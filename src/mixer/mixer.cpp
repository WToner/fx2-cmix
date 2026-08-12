#include "mixer.h"

#include "sigmoid.h"

#include <numeric>
#include <utility>
#include <math.h>
#include <sys/resource.h>
Mixer::Mixer(const std::valarray<float>& inputs,
    const std::valarray<float>& extra_inputs,
    const unsigned long long& context, float learning_rate,
    unsigned int extra_input_size, int combine_mode) : inputs_(inputs),
    extra_inputs_vec_(extra_inputs), extra_inputs_size_(extra_input_size),/*extra_inputs_(extra_input_size),*/ p_(0.5),
    learning_rate_(learning_rate), context_(context), /*max_steps_(1),*/ steps_(0),
    combine_mode_(combine_mode),
    context_base_(inputs.size(), extra_inputs_size_)
    {}

// Cap on distinct gating contexts a mixer will specialise on. Once exceeded,
// every further context collapses onto one shared fallback weight set, so this
// bounds how finely the ensemble can be gated.
#ifndef MIXER_CTX_LIMIT
#define MIXER_CTX_LIMIT 10000
#endif

ContextData* Mixer::GetContextData() {
  ContextData* data;
  unsigned long long limit = MIXER_CTX_LIMIT;
  auto it = context_map_.find(context_); 
  if (context_map_.size() >= limit && it == context_map_.end()) {
    data = &context_base_;
  } else {
    if (it != context_map_.end()) {
      data = &it->second;
    } else {
      auto [it, success] = context_map_.insert({context_, ContextData(inputs_.size(), extra_inputs_size_)});
      data = &it->second;
    }
  }

  return data;
}

float Mixer::Mix() {
  ContextData* data = GetContextData();
  if (combine_mode_ == 1) {
    // Softmax over unconstrained weights, then convex combo of stretched inputs.
    float max_w = data->weights[0];
    for (size_t i = 1; i < data->weights.size(); ++i) {
      if (data->weights[i] > max_w) max_w = data->weights[i];
    }
    float sum = 0;
    // Reuse extra_weights as scratch for softmax probs when unused (L1 has size 0);
    // allocate on stack for generality.
    std::valarray<float> s(data->weights.size());
    for (size_t i = 0; i < data->weights.size(); ++i) {
      s[i] = expf(data->weights[i] - max_w);
      sum += s[i];
    }
    float inv = (sum > 0) ? (1.0f / sum) : 0;
    float p = 0;
    for (size_t i = 0; i < data->weights.size(); ++i) {
      s[i] *= inv;
      p += s[i] * inputs_[i];
    }
    p_ = p;
    return p_;
  }
  float p = 0;
  for (int i = 0; i < inputs_.size(); ++i) {
    p += inputs_[i] * data->weights[i];
  }
  p_ = p;
  float e = 0;
  for (unsigned int i = 0; i < extra_inputs_size_; ++i) {
    e += extra_inputs_vec_[i] * data->extra_weights[i];
  }
  p_ += e;
  return p_;
}

void Mixer::Perceive(int bit) {

#ifndef MIXER_DECAY_FLAT
#define MIXER_DECAY_FLAT 0
#endif
#ifndef MIXER_DECAY_LATE
#define MIXER_DECAY_LATE 0.2f
#endif
#ifndef MIXER_L2_DECAY
#define MIXER_L2_DECAY 0.0f
#endif

  float decay = MIXER_DECAY_LATE;
  if (!(MIXER_DECAY_FLAT)) {
    if ( steps_ < 25000000) {
        decay = 0.3f;
        if ( steps_ < 5000000) { 
            decay = 0.7f;
            if ( steps_ < 1000000)  
                decay = 1.0f;
        }
    }
  } else {
    decay = 1.0f;
  }
  ++steps_;
   
  float update =   learning_rate_ * (Sigmoid::Logistic(p_) - bit);
  // Skip weight updates on near-zero error. Introduced as a speed optimisation,
  // so it is a pure speed/accuracy trade; 0 disables the skip entirely.
#ifndef MIXER_SKIP_EPS
#define MIXER_SKIP_EPS 0.000000000005f
#endif
  if(fabs(update)<(MIXER_SKIP_EPS) && extra_inputs_size_>0) {
      return;
  }
  update = decay * update;
  ContextData* data = GetContextData();

  if (combine_mode_ == 1) {
    // Gradient through softmax: dp/dw_k = s_k * (x_k - p).
    float max_w = data->weights[0];
    for (size_t i = 1; i < data->weights.size(); ++i) {
      if (data->weights[i] > max_w) max_w = data->weights[i];
    }
    float sum = 0;
    std::valarray<float> s(data->weights.size());
    for (size_t i = 0; i < data->weights.size(); ++i) {
      s[i] = expf(data->weights[i] - max_w);
      sum += s[i];
    }
    float inv = (sum > 0) ? (1.0f / sum) : 0;
    for (size_t i = 0; i < data->weights.size(); ++i) {
      s[i] *= inv;
      data->weights[i] -= update * s[i] * (inputs_[i] - p_);
    }
  } else {
    data->weights -= update * inputs_;
    data->extra_weights -= update * extra_inputs_vec_[std::slice(0,extra_inputs_size_,1)];
  }

  if ((MIXER_L2_DECAY) > 0.0f && (steps_ & 1023ULL) == 0) {
    data->weights *= 1.0f - (MIXER_L2_DECAY);
    if (extra_inputs_size_ > 0) {
      data->extra_weights *= 1.0f - (MIXER_L2_DECAY);
    }
  }
}
