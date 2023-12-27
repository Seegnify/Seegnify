/*
 * Copyright 2020-2023 Greg Padiasek. All Rights Reserved.
 */

#ifndef _TRANSFORMER_MODEL_H_
#define _TRANSFORMER_MODEL_H_

#include "main/graph.hh"


using namespace seegnify;


///////////////////////////////////
// Scaled Dot-Product Attention
///////////////////////////////////
class ScaledDotProductAttention : public Function
{
public:
  // q - query vectors
  // k - key vectors
  // v - value vectors
  // trg_size - target and query size
  // seq_size - max sequence size
  // head_size - head size
  // mask - attention mask
  // dropout - dropout probability
  ScaledDotProductAttention(
    Graph& g, Function& q, Function& k, Function& v, Function* mask, 
    int trg_size, int seq_size, int head_size, DTYPE dropout=0.0
  ) :
  Function(g), _q(q), _k(k), _v(v), _mask(mask), _dropout(dropout)
  {
    // L - target lenght, S - sequance lenght, D - embedding dimension
    int L = trg_size;  // _q().rows()
    int S = seq_size;  // _k().rows()
    int D = head_size; // _k().cols()

    // get qk_T attention component [LxS]
    _attention = _graph.new_product(_q, *_graph.new_transpose(_k));

    // attention bias [LxS] (actual dimension updated in forward)
    _bias = _graph.new_constant();

    // scale and add attention mask as bias
    _attention = &(*_attention / sqrt(D) + *_bias);

    // apply row-wise softmax [LxS]
    _attention = _graph.new_rowwise(*_attention, L, S, [&](Function& row) {
      return _graph.new_softmax(row);
    });

    // apply dropout if present
    if (_dropout > 0)
    {
      _attention = _graph.new_dropout(*_attention, _dropout);
    }

    // complete qkv attention []
    _attention = _graph.new_product(*_attention, _v);

    _attention->derivative(_graph.new_iderivative(*this));
  }

  virtual const Tensor& forward()
  {
    if (_value.size() > 0) return _value;

    auto& q = _q();
    auto& k = _k();

    // get input/output dimensions
    int L = q.rows();
    int S = k.rows();

    // initialize default attention mask
    auto& b = _bias->value();
    b = Tensor::Zero(L, S);

    // set custom attention mask
    if (_mask)
    {
      auto& m = _mask->forward();
      DTYPE inf = std::numeric_limits<DTYPE>::infinity();
      b = (m.array() == 0).select(-inf, b);
    }

    _value = _attention->forward();

    return _value;
  }
  
protected:
  Function& _q;
  Function& _k;
  Function& _v;
  Function* _mask;
  Constant* _bias;
  Function* _attention;
  const DTYPE _dropout;
};


///////////////////////////////////
// Multi-Head Attention
///////////////////////////////////
class MultiHeadAttention : public Function
{
public:
  MultiHeadAttention(
    Graph& g, Function& q, Function& k, Function& v, Function* mask,
    int trg_size, int seq_size, int emb_size, int num_heads,
    bool bias = true, DTYPE dropout = 0.0) :
    Function(g)
  {
    const int L = trg_size; // q.rows()
    const int S = seq_size; // k.rows()
    const int E = emb_size; // v.cols()
    const int H = num_heads;
    const int D = emb_size / num_heads;

    // projection matrices
    auto Wq = g.new_variable(E, E, "MHA.Wq");
    auto Wk = g.new_variable(E, E, "MHA.Wk");
    auto Wv = g.new_variable(E, E, "MHA.Wv");
    auto Wo = g.new_variable(E, E, "MHA.Wo");

    // projection bias
    auto bq = (bias) ? g.new_variable(1, E, "MHA.bq") : nullptr;
    auto bk = (bias) ? g.new_variable(1, E, "MHA.bk") : nullptr;
    auto bv = (bias) ? g.new_variable(1, E, "MHA.bv") : nullptr;
    auto bo = (bias) ? g.new_variable(1, E, "MHA.bo") : nullptr;

    auto q_heads = split_heads(linear(q, Wq, bq), H, S, D);
    auto k_heads = split_heads(linear(k, Wk, bk), H, S, D);
    auto v_heads = split_heads(linear(v, Wv, bv), H, S, D);

    std::vector<Function*> heads(num_heads, nullptr);

    for (int i=0; i<num_heads; i++)
    {
      heads[i] = new ScaledDotProductAttention(
        _graph, *q_heads[i], *k_heads[i], *v_heads[i], mask, L, S, H, dropout
      );
      _graph.keep(heads[i]);
    }

    auto& joined = join_heads(heads, S, H);
    _attention = &linear(joined, Wo, bo);

    _attention->derivative(_graph.new_iderivative(*this));
  }

  virtual const Tensor& forward()
  {
    if (_value.size() > 0) return _value;

    // TODO: run attention heads in parallel using thread pool

    _value = _attention->forward();

    return _value;
  }
  
private:
  Function& linear(Function& x, Function* W, Function* b)
  {
    auto y = _graph.new_product(x, *_graph.new_transpose(*W));
    return (b) ? (*y + *_graph.new_broadcast(*b, *y)) : (*y);
  }

  std::vector<Function*> split_heads(
    Function& x, int num_heads, int seq_size, int head_size
  )
  {
    std::vector<Function*> heads(num_heads, nullptr);

    // sequence length, head size
    int S = seq_size;
    int D = head_size;

    // split horizontally
    for (int i=0; i<num_heads; i++)
    {
      heads[i] = _graph.new_split(x, 0,i*D, S,D);
    }

    return heads;
  }

  Function& join_heads(
    const std::vector<Function*>& heads, int seq_size, int head_size
  )
  {
    // sequence length, head size
    int S = seq_size;
    int D = head_size;

    int num_heads = heads.size();

    Function* joined = nullptr;

    // join horizontally
    for (int i=0; i<num_heads; i++)
    {
      // transpose for row-major join
      auto head = _graph.new_transpose(*heads[i]);

      if (joined)
      {
        joined = _graph.new_join(*joined, *head, (i+1)*D,S);
      }
      else
      {
        joined = head;
      }
    }

    // inverse the transpose
    return *_graph.new_transpose(*joined);
  }

private:
  Function *_attention;
};


///////////////////////////////////
// PositionWiseFeedForward
///////////////////////////////////
class PositionWiseFeedForward : public Function
{
public:
  PositionWiseFeedForward(
    Graph& g, Function& x, int emb_size, int ff_size, DTYPE dropout = 0.0) :
    Function(g)
  {
    _y = g.new_linear(x, emb_size, ff_size, true);
    _y = g.new_relu(*_y);
    if (dropout > 0)
    {
      _y = g.new_dropout(*_y, dropout);
    }
    _y = g.new_linear(*_y, ff_size, emb_size, true);

    _y->derivative(g.new_iderivative(*this));
  }

  virtual const Tensor& forward()
  {
    if (_value.size() > 0) return _value;

    // update value
    _value = _y->forward();

    return _value;
  }

private:
  Function* _y;
};


///////////////////////////////////
// PositionalEncoding
///////////////////////////////////
class PositionalEncoding : public Function
{
public:
  PositionalEncoding(
    Graph& g, Function& x, int max_seq_size, int emb_size) :
    Function(g), _x(x)
  {
    Tensor position(max_seq_size, 1);
    Tensor div_term(1, emb_size / 2);

    double term = -(log(10000.0) / emb_size);

    for (int i=0; i<max_seq_size; i++) position(i) = i;
    for (int i=0; i<emb_size; i+=2) div_term(i/2) = exp(i * term);

    Tensor prod = position * div_term;
    _pe.resize(max_seq_size, emb_size);

    _pe(Eigen::all, Eigen::seq(0, emb_size-1, 2)) = prod.array().sin();
    _pe(Eigen::all, Eigen::seq(1, emb_size-1, 2)) = prod.array().cos();

    _x.derivative(g.new_iderivative(*this));
  }

  virtual const Tensor& forward()
  {
    if (_value.size() > 0) return _value;

    auto& x = _x.forward();
    int seq_size = x.rows();

    // update value
    _value = x + _pe(Eigen::seqN(0, seq_size), Eigen::all);

    return _value;
  }

private:
  Tensor _pe;
  Function& _x;
};


///////////////////////////////////
// EncoderLayer
///////////////////////////////////
class EncoderLayer : public Function
{
public:
  EncoderLayer(
    Graph& g, Function& x, Function* mask,
    int seq_size, int emb_size, int num_heads, int ff_size, DTYPE dropout) :
    Function(g)
  {
    _y = new MultiHeadAttention(g, x, x, x, mask,
      seq_size, seq_size, emb_size, num_heads, true, dropout);
    g.keep(_y);

    auto n = g.new_norm(x + *g.new_dropout(*_y, dropout), seq_size, emb_size);

    _y = new PositionWiseFeedForward(g, *n, emb_size, ff_size, dropout);
    g.keep(_y);

    _y = g.new_norm(*n + *g.new_dropout(*_y, dropout), seq_size, emb_size);

    _y->derivative(g.new_iderivative(*this));
  }

  virtual const Tensor& forward()
  {
    if (_value.size() > 0) return _value;

    // update value
    _value = _y->forward();

    return _value;
  }

private:
  Function* _y;
};


///////////////////////////////////
// Transformer
///////////////////////////////////
class Transformer : public Function
{
public:
  Transformer(Graph& g) : Function(g)
  {
  }

  virtual const Tensor& forward()
  {
    if (_value.size() > 0) return _value;

    return _value;
  }

protected:
};


#endif /* _TRANSFORMER_MODEL_H_ */
