/*
 * Copyright (c) 2024 Greg Padiasek
 * Distributed under the terms of the MIT License.
 * See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT
 */

#ifndef _SEEGNIFY_TRAINING_H_
#define _SEEGNIFY_TRAINING_H_

#include "main/graph.hh"
#include "utils/storage.hh"

namespace seegnify {

// Distributed Training
class Training
{
public:
  Training(int worker) : _worker(worker) {}

  virtual ~Training() {}

  virtual void batch_train() = 0;

  // get graph weights
  std::string get_weights()
  {
    // save graph weights
    std::stringstream weights;
    auto curr_vars = _curr.variables();
    write_int(curr_vars.size(), weights);
    for (auto v: curr_vars) write_tensor(v->value(), weights);

    // return graph weights
    return weights.str();
  }

  // set graph weights
  void set_weights(const std::string& weights)
  {
    std::istringstream in(weights);
    int size = read_int(in);

    // create variables
    if (_curr.variables().size() < size)
    {
      for (int i=_curr.variables().size(); i<size; i++) _curr.new_variable();
    }
    if (_prev.variables().size() < size)
    {
      for (int i=_prev.variables().size(); i<size; i++) _prev.new_variable();
    }

    // load weight values
    auto curr_vars = _curr.variables();
    auto prev_vars = _prev.variables();
    for (int i=0; i<size; i++)
    {
      curr_vars[i]->value() = read_tensor(in);
      prev_vars[i]->value() = curr_vars[i]->value();
    }
  }

  // get last weights update
  std::string get_update()
  {
    auto curr_vars = _curr.variables();
    auto prev_vars = _prev.variables();
    if (curr_vars.size() != prev_vars.size())
      throw std::runtime_error("Incompatible number of variables");

    // serialize weight updates
    std::ostringstream out;
    int size = curr_vars.size();
    write_int(size, out);
    for (int i=0; i<size; i++)
    {
      auto& curr = curr_vars[i]->value();
      auto& prev = prev_vars[i]->value();
      write_tensor(curr - prev, out); // save weight increments (+)
    }
    return out.str();
  }

  // apply weight increments (+)
  void upd_weights(const std::string& update)
  {
    auto curr_vars = _curr.variables();
    std::istringstream in(update);
    int size = read_int(in);
    for (int i=0; i<size; i++) curr_vars[i]->value() += read_tensor(in); // (+)
  }

protected:
  Graph& graph() { return _curr; } 
  int worker() { return _worker; }

private:
  Graph _curr;
  Graph _prev;  
  int _worker;
};

} /* namespace */

#endif /*_SEEGNIFY_TRAINING_H_*/
