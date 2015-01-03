#ifndef ITER_PROC_INL_HPP
#define ITER_PROC_INL_HPP
#pragma once
/*!
 * \file cxxnet_iter_proc-inl.hpp
 * \brief definition of preprocessing iterators that takes an iterator and do some preprocessing
 * \author Tianqi Chen
 */
#include "mshadow/tensor.h"
#include "mshadow/tensor_container.h"
#include "data.h"
#include "../utils/utils.h"
#include "../utils/io.h"
#include "../utils/global_random.h"
#include "../utils/thread_buffer.h"

namespace cxxnet {
/*! \brief create a batch iterator from single instance iterator */
class BatchAdaptIterator: public IIterator<DataBatch> {
public:
  BatchAdaptIterator(IIterator<DataInst> *base): base_(base) {
    rand_crop_ = 0;
    rand_mirror_ = 0;
    // skip read, used for debug
    test_skipread_ = 0;
    // scale data
    scale_ = 1.0f;
    // use round roubin to handle overflow batch
    round_batch_ = 0;
    // number of overflow instances that readed in round_batch mode
    num_overflow_ = 0;
    // silent
    silent_ = 0;
    // by default, not mean image file
    name_meanimg_ = "";
    crop_y_start_ = -1;
    crop_x_start_ = -1;
  }
  virtual ~BatchAdaptIterator(void) {
    delete base_;
    out_.FreeSpaceDense();
  }
  virtual void SetParam(const char *name, const char *val) {
    base_->SetParam(name, val);
    if (!strcmp(name, "batch_size"))  batch_size_ = (index_t)atoi(val);
    if (!strcmp(name, "input_shape")) {
      utils::Assert(sscanf(val, "%u,%u,%u", &shape_[2], &shape_[1], &shape_[0]) == 3,
                    "input_shape must be three consecutive integers without space example: 1,1,200 ");
    }
    if (!strcmp(name, "round_batch")) round_batch_ = atoi(val);
    if (!strcmp(name, "rand_crop"))   rand_crop_ = atoi(val);
    if (!strcmp(name, "crop_y_start"))  crop_y_start_ = atoi(val);
    if (!strcmp(name, "crop_x_start"))  crop_x_start_ = atoi(val);
    if (!strcmp(name, "rand_mirror")) rand_mirror_ = atoi(val);
    if (!strcmp(name, "silent"))      silent_ = atoi(val);
    if (!strcmp(name, "divideby"))    scale_ = static_cast<real_t>(1.0f / atof(val));
    if (!strcmp(name, "scale"))       scale_ = static_cast<real_t>(atof(val));
    if (!strcmp(name, "image_mean"))   name_meanimg_ = val;
    if (!strcmp(name, "test_skipread"))    test_skipread_ = atoi(val);
  }
  virtual void Init(void) {
    base_->Init();
    mshadow::Shape<4> tshape = shape_;
    if (tshape[2] == 1 && tshape[1] == 1) {
      tshape[1] = batch_size_; tshape[3] = 1;
    } else {
      tshape[3] = batch_size_;
    }
    out_.AllocSpaceDense(tshape, batch_size_, false);

    if (name_meanimg_.length() != 0) {
      FILE *fi = fopen64(name_meanimg_.c_str(), "rb");
      if (fi == NULL) {
        this->CreateMeanImg();
      } else {
        if (silent_ == 0) {
          printf("loading mean image from %s\n", name_meanimg_.c_str());
        }
        utils::FileStream fs(fi) ;
        meanimg_.LoadBinary(fs);
        fclose(fi);
      }
    }
  }
  virtual void BeforeFirst(void) {
    if (round_batch_ == 0 || num_overflow_ == 0) {
      // otherise, we already called before first
      base_->BeforeFirst();
    } else {
      num_overflow_ = 0;
    }
    head_ = 1;
  }
  virtual bool Next(void) {
    out_.num_batch_padd = 0;

    // skip read if in head version
    if (test_skipread_ != 0 && head_ == 0) return true;
    else this->head_ = 0;

    // if overflow from previous round, directly return false, until before first is called
    if (num_overflow_ != 0) return false;
    index_t top = 0;

    while (base_->Next()) {
      this->SetData(top, base_->Value());
      if (++ top >= batch_size_) return true;
    }
    if (top != 0) {
      if (round_batch_ != 0) {
        num_overflow_ = 0;
        base_->BeforeFirst();
        for (; top < batch_size_; ++top, ++num_overflow_) {
          utils::Assert(base_->Next(), "number of input must be bigger than batch size");
          this->SetData(top, base_->Value());
        }
        out_.num_batch_padd = num_overflow_;
      } else {
        out_.num_batch_padd = batch_size_ - top;
      }
      return true;
    }
    return false;
  }
  virtual const DataBatch &Value(void) const {
    utils::Assert(head_ == 0, "must call Next to get value");
    return out_;
  }
private:
  inline void SetData(int top, const DataInst &d) {
    using namespace mshadow::expr;
    out_.labels[top] = d.label;
    out_.inst_index[top] = d.index;
    if (shape_[2] == 1) {
      out_.data[0][0][top] = d.data[0][0] * scale_;
    } else {
      utils::Assert(d.data.size(2) >= shape_[2] && d.data.size(3) >= shape_[3], "shape constraint");
      mshadow::index_t yy = d.data.size(2) - shape_[2];
      mshadow::index_t xx = d.data.size(3) - shape_[3];
      if (rand_crop_ != 0) {
        yy = utils::NextUInt32(yy + 1);
        xx = utils::NextUInt32(xx + 1);
      } else {
        yy /= 2; xx /= 2;
      }
      if (crop_y_start_ != -1) yy = crop_y_start_;
      if (crop_x_start_ != -1) xx = crop_x_start_;

      if (name_meanimg_.length() == 0) {
        if (rand_mirror_ != 0 && utils::NextDouble() < 0.5f) {
          out_.data[top] = mirror(crop(d.data, out_.data[0][0].shape_, yy, xx)) * scale_;
        } else {
          out_.data[top] = crop(d.data, out_.data[0][0].shape_, yy, xx) * scale_ ;
        }
      } else {
        // substract mean image
        if (rand_mirror_ != 0 && utils::NextDouble() < 0.5f) {
          out_.data[top] = mirror(crop(d.data - meanimg_, out_.data[0][0].shape_, yy, xx)) * scale_;
        } else {
          out_.data[top] = crop(d.data - meanimg_, out_.data[0][0].shape_, yy, xx) * scale_ ;
        }
      }
    }
  }
  inline void CreateMeanImg(void) {
    if (silent_ == 0) {
      printf("cannot find %s: create mean image, this will take some time...\n", name_meanimg_.c_str());
    }
    time_t start = time(NULL);
    unsigned long elapsed = 0;
    size_t imcnt = 1;

    utils::Assert(base_->Next(), "input empty");
    meanimg_.Resize(base_->Value().data.shape_);
    mshadow::Copy(meanimg_, base_->Value().data);
    while (base_->Next()) {
      meanimg_ += base_->Value().data; imcnt += 1;
      elapsed = (long)(time(NULL) - start);
      if (imcnt % 1000 == 0 && silent_ == 0) {
        printf("\r                                                               \r");
        printf("[%8lu] images processed, %ld sec elapsed", imcnt, elapsed);
        fflush(stdout);
      }
    }
    meanimg_ *= (1.0f / imcnt);
    utils::StdFile fo(name_meanimg_.c_str(), "wb");
    meanimg_.SaveBinary(fo);
    if (silent_ == 0) {
      printf("save mean image to %s..\n", name_meanimg_.c_str());
    }
    base_->BeforeFirst();
  }
private:
  // base iterator
  IIterator<DataInst> *base_;
  // batch size
  index_t batch_size_;
  // input shape
  mshadow::Shape<4> shape_;
  // output data
  DataBatch out_;
  // on first
  int head_;
  // skip read
  int test_skipread_;
  // silent
  int silent_;
  // scale of data
  real_t scale_;
  // whether we do random cropping
  int rand_crop_;
  // whether we do random mirroring
  int rand_mirror_;
  // use round roubin to handle overflow batch
  int round_batch_;
  // whether we do nonrandom croping
  int crop_y_start_;
  // whether we do nonrandom croping
  int crop_x_start_;
  // number of overflow instances that readed in round_batch mode
  int num_overflow_;
  // mean image, if needed
  mshadow::TensorContainer<cpu, 3> meanimg_;
  // mean image file, if specified, will generate mean image file, and substract by mean
  std::string name_meanimg_;
}; // class BatchAdaptIterator
}; // namespace cxxnet

namespace cxxnet {
/*! \brief thread buffer iterator */
class ThreadBufferIterator: public IIterator< DataBatch > {
public :
  ThreadBufferIterator(IIterator<DataBatch> *base) {
    silent_ = 0;
    itr.get_factory().base_ = base;
    itr.SetParam("buffer_size", "2");
  }
  virtual ~ThreadBufferIterator() {
    itr.Destroy();
  }
  virtual void SetParam(const char *name, const char *val) {
    if (!strcmp(name, "silent")) silent_ = atoi(val);
    itr.SetParam(name, val);
  }
  virtual void Init(void) {
    utils::Assert(itr.Init(), "iterator init fail") ;
    if (silent_ == 0) {
      printf("ThreadBufferIterator: buffer_size=%d\n", itr.buf_size);
    }
  }
  virtual void BeforeFirst() {
    itr.BeforeFirst();
  }
  virtual bool Next() {
    if (itr.Next(out_)) {
      return true;
    } else {
      return false;
    }
  }
  virtual const DataBatch &Value() const {
    return out_;
  }
private:
  struct Factory {
  public:
    IIterator< DataBatch > *base_;
  public:
    Factory(void) {
      base_ = NULL;
    }
    inline void SetParam(const char *name, const char *val) {
      base_->SetParam(name, val);
    }
    inline bool Init() {
      base_->Init();
      utils::Assert(base_->Next(), "ThreadBufferIterator: input can not be empty");
      oshape_ = base_->Value().data.shape_;
      batch_size_ = base_->Value().batch_size;
      base_->BeforeFirst();
      return true;
    }
    inline bool LoadNext(DataBatch &val) {
      if (base_->Next()) {
        val.CopyFromDense(base_->Value());
        return true;
      } else {
        return false;
      }
    }
    inline DataBatch Create(void) {
      DataBatch a; a.AllocSpaceDense(oshape_, batch_size_);
      return a;
    }
    inline void FreeSpace(DataBatch &a) {
      a.FreeSpaceDense();
    }
    inline void Destroy() {
      if (base_ != NULL) delete base_;
    }
    inline void BeforeFirst() {
      base_->BeforeFirst();
    }
  private:
    mshadow::index_t batch_size_;
    mshadow::Shape<4> oshape_;
  };
private:
  int silent_;
  DataBatch out_;
  utils::ThreadBuffer<DataBatch, Factory> itr;
}; // class ThreadBufferIterator
}; // namespace cxxnet
#endif
