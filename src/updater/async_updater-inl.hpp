#ifndef CXXNET_UPDATER_ASYNC_UPDATER_INL_HPP_
#define CXXNET_UPDATER_ASYNC_UPDATER_INL_HPP_
/*!
 * \file sgd_updater-inl.hpp
 * \brief implementation of asynchronize updater using SGD
 * \author Tianqi Chen
 */
#include <mshadow/tensor.h>
#include <mshadow-ps/ps.h>
#include "./updater.h"
#include "../utils/timer.h"
namespace cxxnet {
namespace updater {
template<typename xpu>
class AsyncUpdater: public IAsyncUpdater<xpu> {
 public:
  AsyncUpdater(int data_key, int devid, int priority,
               mshadow::Tensor<xpu, 2> w, mshadow::Tensor<xpu, 2> dw,
               layer::LayerType layer_type, const char *tag,
               mshadow::ps::IParamServer<xpu, real_t> *pserver,
               IUpdater<xpu> *updater)
      : data_key(data_key), devid(devid),
        priority(priority), w(w), dw(dw),
        layer_type(layer_type), tag(tag),
        pserver(pserver), updater(updater), tnode(false) {
    fullc_gather = 0;
    local_batch_size = 0;
    total_batch_size = 0;
    total = 0;
  }
  virtual ~AsyncUpdater(void) {
    delete updater;
  }
  virtual void Init(void) {
    updater->Init();
    if (fullc_gather != 0 && pserver != NULL) {
      char name[32];
      sprintf(name, "push_op[%d]", data_key);
      pserver->SetParam(name, "gather");
    }
  }
  virtual void SetStream(mshadow::Stream<xpu> *stream) {
    updater->SetStream(stream);
    tnode.set_stream(stream);
  }
  virtual void BeforeBackprop(const std::vector<layer::Node<xpu>*> &nodes_in,
                              const std::vector<layer::Node<xpu>*> &nodes_out) {
    if (fullc_gather != 0) {
      utils::Check(nodes_in.size() == 1, "fullc_gather can only work with fullc");
      utils::Check(nodes_out.size() == 1, "fullc_gather can only work with fullc");
      mshadow::Tensor<xpu, 2> in = nodes_in[0]->mat();
      mshadow::Tensor<xpu, 2> out = nodes_out[0]->mat();
      num_in = in.size(1); num_out = out.size(1);
      tnode.Resize(mshadow::Shape2(total_batch_size, num_in + num_out));      
      // manually hslice
      mshadow::Tensor<xpu, 2> tin(tnode.dptr_,
                                  mshadow::Shape2(total_batch_size, num_in),
                                  tnode.stride_, tnode.stream_);
      mshadow::Tensor<xpu, 2> tout(tnode.dptr_ + num_in,
                                   mshadow::Shape2(total_batch_size, num_out),
                                   tnode.stride_, tnode.stream_);
      local_batch_size = in.size(0);
      utils::Check(local_batch_size <= total_batch_size,
                   "local_batch_size bigger than total_batch_size");
      utils::Check(total_batch_size % local_batch_size == 0,
                   "when you use fullc_gather mode, the batch_size "\
                   "must be multiple of number of devices");
      mshadow::Copy(tin.Slice(0, local_batch_size), in, tnode.stream_);
      mshadow::Copy(tout.Slice(0, local_batch_size), out, tnode.stream_);      
    }
  }
  virtual void AfterBackprop(bool do_update, long epoch) {
    if (fullc_gather == 0) {
      if (do_update && pserver == NULL) {
        updater->Update(epoch); return;
      }
      if (do_update) {
        this->update_epoch = epoch;
        pserver->Push(dw, data_key, devid, priority);
        pserver->PullReq(dw, data_key, devid, priority,
                         ApplyUpdate_,
                         this);
      }
    } else {
      this->do_update = do_update;
      this->update_epoch = epoch;
      if (do_update && pserver == NULL) {
        this->CalcDelta(dw.stream_);
        updater->Update(epoch); return;
      }
      pserver->Push(tnode.Slice(0, local_batch_size), data_key, devid, priority);
      pserver->PullReq(tnode, data_key, devid, priority,
                       ApplyGatherUpdate_, this);
    }
  }
  virtual void UpdateWait(void) {
    if (pserver == NULL) return;
    double begin = cxxnet::utils::GetTime();
    pserver->PullWait(data_key, devid);
    double end = cxxnet::utils::GetTime();
    total += end - begin;
  }
  virtual void StartRound(int round) {
    printf("Round %d: %f\n", round, total);
    updater->StartRound(round);
  }
  virtual void SetParam(const char *name, const char *val) {
    updater->SetParam(name, val);
    if (!strcmp(name, "fullc_gather")) {
      if (tag == "wmat" && layer_type == layer::kFullConnect) {
        fullc_gather = atoi(val);
      }
    }
    if (!strcmp(name, "batch_size")) {
      total_batch_size = static_cast<index_t>(atoi(val));
    }
  }
  virtual void ApplyVisitor(typename IUpdater<xpu>::IVisitor *pvisitor) {
    updater->ApplyVisitor(pvisitor);
  }

 protected:
  inline void CalcDelta(mshadow::Stream<xpu> *stream) {
    dw.set_stream(stream);
    mshadow::Tensor<xpu, 2> tin(tnode.dptr_,
                                mshadow::Shape2(total_batch_size, num_in),
                                tnode.stride_, stream);
    mshadow::Tensor<xpu, 2> tout(tnode.dptr_ + num_in,
                                 mshadow::Shape2(total_batch_size, num_out),
                                 tnode.stride_, stream);
    dw += dot(tout.T(), tin);
  }
  inline static void ApplyUpdate_(mshadow::Stream<xpu> *stream, void *arg) {
    AsyncUpdater<xpu> *up = static_cast<AsyncUpdater<xpu>*>(arg);
    up->updater->SetStream(stream);
    up->updater->Update(up->update_epoch);
  }
  
  inline static void ApplyGatherUpdate_(mshadow::Stream<xpu> *stream, void *arg) {
    AsyncUpdater<xpu> *up = static_cast<AsyncUpdater<xpu>*>(arg);
    up->CalcDelta(stream);
    if (up->do_update) {
      up->updater->SetStream(stream);
      up->updater->Update(up->update_epoch);
    }
  }
  int data_key, devid, priority;
  long update_epoch;
  mshadow::Tensor<xpu, 2> w, dw;
  layer::LayerType layer_type;
  std::string tag;
  mshadow::ps::IParamServer<xpu, real_t> *pserver;
  IUpdater<xpu> *updater;
  // the following data structure are used to support fullc_gather
  // use gather update for fullc layer
  int fullc_gather;
  // whether do update this round
  bool do_update;
  // number of input and output nodex
  index_t num_in, num_out;
  // the total batch_size across nodes
  index_t local_batch_size, total_batch_size;
  // temporal result 
  mshadow::TensorContainer<xpu, 2> tnode; 
  double total; 
};
}  // updater
}  // cxxnet
#endif
