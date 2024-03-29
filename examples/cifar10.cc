/*
 * Copyright 2020-2021 Greg Padiasek and Seegnify <http://www.seegnify.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cifar10.hh"

#include "main/optimizer.hh"
#include "utils/training.hh"
#include "utils/storage.hh"

#include "cifar/cifar10_reader.hpp"
#include "cifar10.hh"

///////////////////////////////////
// training instance implementation
///////////////////////////////////
class CIFAR10Client : public Training
{
public:
  CIFAR10Client(int worker) : Training(worker)
  {
    std::cout << "CIFAR10 training " << worker << std::endl;

    // load data
    _data = cifar::read_dataset<std::vector, std::vector, DTYPE, uint8_t>
    ("./data/cifar10/cifar-10-batches-bin");

    // create graph
    Graph& g = graph();
    _model = new CIFAR10Model(g);    

    // optimizer
    _optimizer = new Adam(g.variables(), 0.001);

    // loss
    _y_hat = g.new_constant(OUTPUT, 1);
    auto& ce = -*_y_hat * *g.new_log_softmax(_model->output_logits());
    _loss = g.new_sum(ce);

    // counters
    _steps = 0;
    _positive = 0;

    // data index
    _training.resize(_data.training_images.size());
    for (int i=_data.training_images.size()-1; i>=0; i--) _training[i] = i;
  }

  ~CIFAR10Client()
  {
    delete _optimizer;
  }

  void set_input(Tensor& in, Tensor& out, std::vector<DTYPE>& image, int label)
  {
    in.block(0,0, INPUT,1) = Eigen::Map<Tensor>(image.data(), INPUT, 1);

    in.block(0,0, INPUT,1) /= (in.block(0,0, INPUT,1).norm() + EPSILON);

    for (int i=0; i<OUTPUT; i++) out(i, 0) = (label == i);
  }

  // get output
  int get_output(const Tensor& out)
  {
    int max_i = -1;
    DTYPE max_v = 0;

    for (int i=0; i<OUTPUT; i++)
    {
      if (out(i, 0) > max_v)
      {
        max_v = out(i, 0);
        max_i = i;
      }
    }

    return max_i;
  }

  virtual void batch_train()
  {
    // restore references
    auto& g = graph();
    auto& x = _model->input();
    auto& y = _model->output();
    auto& y_hat = *_y_hat;
    auto& loss = *_loss;
    auto& opt = *_optimizer;

    int batch_size = 100;
    int positive = 0;
    _steps++;

    // data index
    graph().random().shuffle(_training.begin(), _training.end(), batch_size);

    // batch train
    for (int i=0; i<batch_size; i++)
    {
      int ir = _training[i];
      auto& image = _data.training_images[ir];
      auto label = _data.training_labels[ir];

      g.recache();
      set_input(x.value(), y_hat.value(), image, label);
      g.backward(loss, loss());

      auto predl = get_output(y());
      if (predl == label) _positive++;
    }

    // update weights
    opt.update();
    g.zero_grad();

    int log_step = 1;
    if (_steps % log_step == 0)
    {
      std::cout
          << "batch " << _steps
          << ", success " << (float)_positive/log_step/batch_size
          << std::endl;
      _positive = 0;
    }

    int valid_step = 1000;
    if (_steps % valid_step == 0)
    {
      std::cout << "validation success "
                << validate(g, x, y, y_hat)
                << std::endl;
    }
  }

  float validate(Graph& g, Constant& x, Function& y, Constant& y_hat)
  {
    int positive = 0;
    int size = _data.test_images.size();

    for (int i=0; i<size; i++)
    {
      auto& image = _data.test_images[i];
      auto label = _data.test_labels[i];

      g.recache();
      set_input(x.value(), y_hat.value(), image, label);

      auto predl = get_output(y());
      if (predl == label) positive++;
    }

    return (float)positive/size;
  }

private:
  CIFAR10Model *_model;
  Constant *_y_hat;
  Function *_loss;
  Optimizer *_optimizer;
  int _steps;
  int _positive;
  std::vector<int> _training;

  // cifar data
  cifar::CIFAR10_dataset<std::vector, std::vector<DTYPE>, uint8_t> _data;
};

///////////////////////////////////
extern "C" { // export C signatures
///////////////////////////////////

DLL_EXPORT Training* create(int idx)
{
  return new CIFAR10Client(idx);
}

DLL_EXPORT void destroy(Training* ptr)
{
  delete (CIFAR10Client*)ptr;
}

///////////////////////////////////
} // export C signatures
///////////////////////////////////

