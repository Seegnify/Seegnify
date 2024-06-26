/*
 * Copyright (c) 2024 Greg Padiasek
 * Distributed under the terms of the MIT License.
 * See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT
 */

#include <iostream>
#include <thread>
#include <csignal>
#include <chrono>
#include <queue>

#include <unsupported/Eigen/FFT>

#include "main/graph.hh"
#include "main/optimizer.hh"
#include "unittest.hh"
#include "storage.hh"
#include "image.hh"
#include "imageFP.hh"
#include "painter.hh"
#include "rlenv.hh"

using namespace seegnify;

/////////////////////////////////////////
// Helper functions
/////////////////////////////////////////

void print(const std::string& name, const Tensor& tensor)
{
  std::cout << name
  << " [" << tensor.rows() << " x " << tensor.cols() << "]"
  << std::endl;
  std::cout << tensor << std::endl;
}

void print(const std::string& name, const SparseTensor& tensor)
{
  std::cout << name
  << " [" << tensor.rows() << " x " << tensor.cols() << "]"
  << std::endl;
  std::cout << tensor << std::endl;
}

void print(const std::string& name, Function& f)
{
  print(name, f.forward());
}

void print(const Graph& g, bool values = false)
{
  auto vars = g.named_variables();

  for (const auto& it: vars)
  {
    auto name = it.first;
    auto var = it.second;

    // print name and varaible value
    auto& tensor = var->forward();
    std::cout << "node[" << name << "] "
    << " [" << tensor.rows() << " x " << tensor.cols() << "]"
    << std::endl;
    if (values)
    {
      std::cout << tensor << std::endl;
    }
  }
}

void print(const std::string& name, const Image& image)
{
  std::cout << name
  << " [" << image.rows() << " x " << image.cols() << "]"
  << " x " << (int) image.channels()
  << std::endl;
}

Tensor tensor(int rows, int cols, std::initializer_list<DTYPE> vals)
{
  Tensor t(rows, cols);
  auto it = vals.begin();

  for (int i=0; it != vals.end(); it++, i++)
    t(i / cols, i % cols) = *it;

  return t;
}

/////////////////////////////////////////
// Test cases
/////////////////////////////////////////

void test_eigen_fft()
{
  TEST_BEGIN("FFT/IFFT")

  // define wave parameters
  DTYPE AM = 5;  // amplitude
  DTYPE FR = 10; // frequency [Hz]
  DTYPE PH = 30; // phase [degree]
  DTYPE OC = 32; // oversampling coefficient

  // define sample steps
  DTYPE step = 1.0 / (FR * OC); // sampling time step
  int steps = round(2.0 / step); // number of samples in 2 seconds

  // create wave positions
  std::vector<DTYPE> wave;
  for (int i=0; i<steps; i++)
  {
    auto t = i * step;
    wave.push_back(AM * cos(2 * M_PI * FR * t + PH * M_PI / 180));
  }

  // FFT params
  int N = 256; // N < sampling frequency FR * OC
  Eigen::FFT<DTYPE> fft;

  std::vector<DTYPE> timevec(wave.data(), wave.data() + N);
  std::vector<std::complex<DTYPE>> freqvec;
  std::vector<DTYPE> amplitude;
  std::vector<DTYPE> phase;

  // convert time domain to frequency domain
  fft.fwd(freqvec, timevec);

  // expected frquency, amplitude and phase
  DTYPE y_f = -1;
  DTYPE y_a = -1;
  DTYPE y_p = -1;

  // calculate amplitudes
  DTYPE fstep = FR * OC / N; // sampling frequency / N
  for (int i=0; i<N/2; i++)
  {
    amplitude.push_back(std::abs(freqvec[i]) / N);
    if (amplitude.back() > 0.1)
    {
      y_f = i * fstep;
      y_a = 2 * amplitude.back();
    }
  }

  // validate prominent frequency and amplitude
  ASSERT(std::abs(y_f - FR) < 1e-4);
  ASSERT(std::abs(y_a - AM) < 1e-4);

  // calculate angles, mask frquencies of low amplitude 
  DTYPE threshold = *std::max_element(
    amplitude.begin(), amplitude.end()) / 10000;
  for (int i=0; i<N/2; i++)
  {
    auto e = freqvec[i];
    auto a = std::atan2(std::imag(e), std::real(e)) * 180 / M_PI;
    phase.push_back((std::abs(e) > threshold) ? a : 0);
    if (phase.back() > 0.1)
    {
      y_f = i * fstep;
      y_p = phase.back();
    }
  }

  // validate prominent frequency and phase
  ASSERT(std::abs(y_f - FR) < 1e-4);
  ASSERT(std::abs(y_p - PH) < 1e-4);

  // convert frequency domain to time domain
  std::vector<DTYPE> timevec2;
  fft.inv(timevec2, freqvec);

  Tensor fft_in = TensorMap(timevec.data(),N,1);
  Tensor fft_out = TensorMap(timevec2.data(),N,1);

  // assert idenyity IFFT(FTT(x)) == x
  ASSERT(fft_in.isApprox(fft_out, 1e-6));

  TEST_END()
}

void test_audio_file()
{
  TEST_BEGIN("Audio File")

  int num_channels = 1;
  int sample_rate = 12000;
  std::vector<DTYPE> samples;

  // define tone wave parameters
  DTYPE AM = 0.8;  // amplitude
  DTYPE FR = 440; // frequency [Hz]
  DTYPE PH = 10; // phase [degree]

  // define sample steps
  DTYPE step = 1.0 / sample_rate; // sampling time step
  int steps = round(1.0 / step); // number of samples in 1 seconds

  // create wave positions
  for (int i=0; i<steps; i++)
  {
    auto t = i * step;
    samples.push_back(AM * sin(2 * M_PI * FR * t + PH * M_PI / 180));
  }

  // write tone audio to file
  save_audio("/tmp/in.wav", samples, num_channels, sample_rate);

  // read input audio from file
  load_audio("/tmp/in.wav", samples, num_channels, sample_rate);

  ASSERT(num_channels == 1);

  // fft window size rounded to the nearest power of 2
  DTYPE fft_window = 0.30; // 30 ms
  int N = pow(2, ceil(log(sample_rate * fft_window) / log(2)));

  ASSERT(N > 4);
  ASSERT(N <= samples.size());

  // add padded tail so that samples.size() % N == 0
  int padding = samples.size() % N;
  samples.resize(samples.size() + padding);

  // output time domain
  std::vector<DTYPE> output(samples.size(), 0);

  // iterate over windows with 50% overlap
  for (int i=0; i<=samples.size()-N; i+=N/2)
  {
    std::vector<DTYPE> intime(samples.data() + i, samples.data() + i + N);
    std::vector<std::complex<DTYPE>> freqvec;

    // convert time domain to frequency domain
    Eigen::FFT<DTYPE> fft;
    fft.fwd(freqvec, intime);

    // process the window in some way
    for (auto& e: freqvec) e = std::complex<DTYPE>(e.real(), 0);

    // convert frequency domain to time domain
    std::vector<DTYPE> outtime;
    fft.inv(outtime, freqvec);

    // combine windows
    for (int j=0; j<N; j++) output[i+j] += outtime[j];
  }

  // correct/reduce amplitude in overalped regions
  for (int i=N/2; i<output.size()-N/2; i++) output[i] /= 2;

  // remove padded tail
  output.resize(output.size() - padding);

  // write output audio to file
  save_audio("/tmp/out.wav", output, num_channels, sample_rate);

  TEST_END()
}

void test_image_file()
{
  TEST_BEGIN("Image Scale & Crop")

  int rows = 100;
  int cols = 200;
  int channels = 3;

  Image im(rows, cols, channels);
  ASSERT(im.rows() == rows);
  ASSERT(im.cols() == cols);
  ASSERT(im.channels() == channels);

  // set background color
  auto data = im.data();
  memset(data, 128, rows * cols * channels / 2);
  memset(data + rows * cols * channels / 2, 64, rows * cols * channels / 2);

  // scale nearest
  int rows_nearest = 150;
  int cols_nearest = 88;
  auto im_nearest = im.scale(rows_nearest, cols_nearest, Image::INTERPOLATE_NEAREST);
  ASSERT(im_nearest.rows() == rows_nearest);
  ASSERT(im_nearest.cols() == cols_nearest);
  ASSERT(im_nearest.channels() == channels);

  // scale bilinear
  int rows_bilinear = 150;
  int cols_bilinear = 88;
  auto im_bilinear = im.scale(rows_bilinear, cols_bilinear, Image::INTERPOLATE_BILINEAR);
  ASSERT(im_bilinear.rows() == rows_bilinear);
  ASSERT(im_bilinear.cols() == cols_bilinear);
  ASSERT(im_bilinear.channels() == channels);

  // crop image
  int r_cropped = -20;
  int c_cropped = 20;
  int rows_cropped = 150;
  int cols_cropped = 88;
  auto im_cropped = im.crop(r_cropped, c_cropped, rows_cropped, cols_cropped);
  ASSERT(im_cropped.rows() == rows_cropped);
  ASSERT(im_cropped.cols() == cols_cropped);
  ASSERT(im_cropped.channels() == channels);

  TEST_END()

  TEST_BEGIN("Image Save & Load")
  int rows = 100;
  int cols = 200;
  int channels = 3;
  Image im(rows, cols, channels);

  // set background color
  auto data = im.data();
  memset(data, 128, rows * cols * channels / 2);
  memset(data + rows * cols * channels / 2, 64, rows * cols * channels / 2);

  int rows_nearest = 150;
  int cols_nearest = 88;
  auto im_nearest = im.scale(rows_nearest, cols_nearest, Image::INTERPOLATE_NEAREST);

  save_image("/tmp/seegnify-unittest.bmp", im_nearest.data(),
    im_nearest.rows(), im_nearest.cols(), im_nearest.channels() * 8);
  im.load("/tmp/seegnify-unittest.bmp");
  ASSERT(im_nearest.rows() == im.rows());
  ASSERT(im_nearest.cols() == im.cols());
  ASSERT(im_nearest.channels() == im.channels());

  int rows_bilinear = 150;
  int cols_bilinear = 88;
  auto im_bilinear = im.scale(rows_bilinear, cols_bilinear, Image::INTERPOLATE_BILINEAR);

  im_bilinear.save("/tmp/seegnify-unittest.bmp");
  im.load("/tmp/seegnify-unittest.bmp");
  ASSERT(im_bilinear.rows() == im.rows());
  ASSERT(im_bilinear.cols() == im.cols());
  ASSERT(im_bilinear.channels() == im.channels());

  TEST_END()

  TEST_BEGIN("Image Normalization")
  int rows = 100;
  int cols = 200;
  int channels = 3;
  Image im(rows, cols, channels);

  // set background color
  auto data = im.data();
  memset(data, 128, im.size() / 2);
  memset(data + im.size() / 2, 64, im.size() / 2);

  ASSERT(im.data()[0] == 128);
  ASSERT(im.data()[im.size()-1] == 64);

  Image norm = im.norm();

  ASSERT(norm.data()[0] == 255);
  ASSERT(norm.data()[im.size()-1] == 0);

  TEST_END()

  TEST_BEGIN("Image Move")

  int rows = 100;
  int cols = 200;

  Image img(rows, cols);

  Image img2;
  img2 = std::move(img);

  ASSERT(img.data() == nullptr);
  ASSERT(img2.data() != nullptr);

  TEST_END()
}

void test_eigen_matrix()
{
  TEST_BEGIN("Matrix Map")  

  // copy form external
  //DTYPE X[] = {1,4,2,5,3,6}; // col major
  DTYPE X[] = {1,2,3,4,5,6}; // row major;
  Tensor x1 = Eigen::Map<Tensor>(X, 2, 3);
  //X[1] = 19; // col major
  X[3] = 19; // row major
  Tensor y1(2, 3);
  y1 << 1,2,3,4,5,6;
  ASSERT(x1 == y1);

  // pointer to external 
  Eigen::Map<Tensor> x2(X, 2, 3);
  //X[2] = 20; // col major
  X[1] = 20; // row major
  Tensor y2(2, 3);
  y2 << 1,20,3,19,5,6;
  ASSERT(x2 == y2);
  
  // read tensor buffer
  Tensor x3(2, 3);
  x3 << 1,2,3,4,5,6;
  DTYPE* Y3 = x3.data();
  //Y3[1] = 21; // col major
  Y3[3] = 21; // row major
  Tensor y3(2, 3);
  y3 << 1,2,3,21,5,6;
  ASSERT(x3 == y3);

  // write tensor buffer
  Tensor x4(2, 3);
  x4 << 1,2,3,4,5,6;
  Tensor y4(2, 3);
  y4 << 2,3,4,5,6,77;
  DTYPE* Y4 = x4.data();
  Eigen::Map<Tensor>(Y4, 2, 3) = y4;
  ASSERT(x4 == y4);

  TEST_END()

  TEST_BEGIN("Matrix Storage")  

  // size
  int IN = 4;

  Tensor A = Tensor::Random(IN,1);
  Tensor B = Tensor::Random(IN,IN);

  std::stringstream store;

  write_tensor(A, store);
  write_tensor(B, store);

  auto newA = read_tensor(store);
  auto newB = read_tensor(store);

  ASSERT(A == newA)
  ASSERT(B == newB)

  TEST_END()
}

void test_random_numbers()
{
  TEST_BEGIN("Random Choice")

  RNG rng;

  std::vector<DTYPE> dist;
  dist.push_back(0.1);
  dist.push_back(0.2);
  dist.push_back(0.4);
  dist.push_back(0.5);

  int N = 1000;
  std::vector<int> hist(dist.size(), 0);
  for (int i=0; i<N; i++)
  {
    hist[rng.discrete_choice(dist.begin(), dist.end())] += 1;
  }

  int prev = 0;
  for (int i=0; i<hist.size(); i++)
  {
    ASSERT(prev < hist[i])
    prev = hist[i];
  }

  TEST_END()

  TEST_BEGIN("Random Choice M of N")

  std::vector<DTYPE> choices = {0,1,2,3,4,5,6,7,8,9};
  auto choices0 = choices;

  RNG rng;

  rng.shuffle(choices.begin(), choices.end(), 3);
  ASSERT(choices0 != choices)

  TEST_END()
}

void test_discount_reward()
{
  TEST_BEGIN("Discount Reward")

  int N = 5;

  Tensor reward(N, 1);
  Tensor target(N, 1);

  reward << 0.1, 0, 0, 0, 1;
  std::vector<DTYPE> v(reward.data(), reward.data() + reward.size()); 

  // gamma 1.0
  target << 1.1, 1, 1, 1, 1;

  auto d = discount_reward(v, 1.0);

  reward = TensorMap(d.data(), N, 1);
  ASSERT(reward == target)

  // gamma 0.99
  target << 1.0606, 0.970299, 0.9801, 0.99, 1;

  d = discount_reward(v, 0.99);

  reward = TensorMap(d.data(), N, 1);
  ASSERT(reward.isApprox(target, 0.001))

  TEST_END()
}

void test_cosine_similarity()
{
  TEST_BEGIN("Cosine Similarity")

  Tensor a(5, 1);
  a << 1,2,3,4,5;

  Tensor b(5, 4);
  b.block(0,0, 5,1) = 2 * a;
  b.block(0,1, 5,1) = -a;
  b.block(0,2, 5,1) = 0 * a;
  b.block(0,3, 5,1) << 1,1,-2,2,-3;

  auto cs = cosine_similarity(a.transpose(), b);

  ASSERT(abs(cs(0) - 1.0) < EPSILON)
  ASSERT(abs(cs(1) + 1.0) < EPSILON)
  ASSERT(cs(2) == 0)
  ASSERT(cs(3) < -EPSILON)
  ASSERT(cs(3) > EPSILON - 1.0)

  TEST_END()
}

void test_function_negative()
{
  TEST_BEGIN("Function Negative")
  Graph g;

  auto& c = *g.new_constant(2,3);
  c.value() << -1,2,-3, 4,-5,6;
  auto& mc = -c;

  Tensor mc_hat(2,3);
  mc_hat << 1,-2,3, -4,5,-6;
  ASSERT(mc() == mc_hat)

  TEST_END()
}

void test_function_names()
{
  TEST_BEGIN("Function Names")
  Graph g;

  auto& c = *g.new_constant(2,3);
  c.value() << -1,2,-3, 4,-5,6;
  auto& mc = -c;

  const char* name = "Negative Constant";
  g.name(&mc, name);

  auto named_mc = g.function(name);
  ASSERT(named_mc == &mc);

  TEST_END()
}

void test_constant()
{
  TEST_BEGIN("Constant")
  Graph g;

  // size
  int IN = 2;

  // x vector
  Constant x(g, IN, 1);
  x.value() << 1, 2;
  ASSERT(x.forward().rows() == IN);
  ASSERT(x.forward().cols() == 1);
  ASSERT(x.value().rows() == IN);
  ASSERT(x.value().cols() == 1);

  // x_hat
  Tensor x_hat(IN, 1);
  x_hat << 1, 2;
  ASSERT(x_hat == x.forward());

  TEST_END()
}

void test_variable()
{
  TEST_BEGIN("Variable")
  Graph g;

  // size
  int IN = 2;
  int OUT = 4;

  // x
  Variable x(g, IN, OUT);
  x.value() << 1, 2,
               3, 4,
               5, 6,
               7, 8;
  ASSERT(x.forward().rows() == IN);
  ASSERT(x.forward().cols() == OUT);
  ASSERT(x.value().rows() == IN);
  ASSERT(x.value().cols() == OUT);

  // x_hat
  Tensor x_hat(IN, OUT);
  x_hat << 1, 2,
           3, 4,
           5, 6,
           7, 8;
  ASSERT(x_hat == x.forward());

  TEST_END()
}

void test_numerical_derivative()
{
  TEST_BEGIN("Numerical Derivative")

  // size
  int IN = 2;
  int OUT = 3;
  Graph g;

  // x
  auto& x = *g.new_variable(1, IN);
  x.value() << 1, 2;

  // F = xW.T + b
  auto& y = *g.new_linear(x, IN, OUT);
  
  // W
  auto& W = y.W();
  W.value() << 1, 2,
               3, 4,
               5, 6;

  // b
  auto& b = y.b();
  b.value() << 1, 2, 3;

  // dFdx
  Tensor dFdx = g.dFdX(y, x);
  ASSERT(dFdx.rows() == x.value().rows())
  ASSERT(dFdx.cols() == x.value().cols())

  // dFdx_hat = W
  Tensor dFdx_hat = W.value().colwise().sum().transpose();
  ASSERT(dFdx.isApprox(dFdx_hat, 0.01))

  // dFdW
  Tensor dFdW = g.dFdX(y, W);
  ASSERT(dFdW.rows() == W.value().rows())
  ASSERT(dFdW.cols() == W.value().cols())

  // dFdW_hat = x
  Tensor dFdW_hat(OUT, IN);
  dFdW_hat << 1, 2,
              1, 2,
              1, 2,
  ASSERT(dFdW.isApprox(dFdW_hat, 0.01))

  // dFdb
  Tensor dFdb = g.dFdX(y, b);
  ASSERT(dFdb.rows() == b.value().rows())
  ASSERT(dFdb.cols() == b.value().cols())

  // dFdb_hat = 1
  Tensor dFdb_hat = Tensor::Ones(OUT, 1);
  ASSERT(dFdb.isApprox(dFdb_hat, 0.01))

  TEST_END()
}

void test_back_propagation()
{
  TEST_BEGIN("Back Propagation")

  // size
  int IN = 2;
  int OUT = 3;
  Graph g;

  // x
  auto& x = *g.new_variable(1, IN);
  x.value() << 1, 2;

  // diable backprop on X
  x.backprop(false);

  // F = Wx + b
  auto& y = *g.new_linear(x, IN, OUT);
  
  // W
  auto& W = y.W();
  W.value() << 1, 2,
               3, 4,
               5, 6;

  // b
  auto& b = y.b();
  b.value() << 1,
               2,
               3;

  // F
  auto& F = y.forward();
  ASSERT(F.rows() == 1)
  ASSERT(F.cols() == OUT)

  y.gradient() = Tensor::Ones(1, OUT);
  auto& dFdW = W.backward();
  auto& dFdb = b.backward();
  auto& dFdx = x.backward();

  // dFdx
  ASSERT(dFdx.rows() == x.value().rows())
  ASSERT(dFdx.cols() == x.value().cols())
  ASSERT(dFdx == Tensor::Zero(1, IN))

  // dFdW num
  Tensor dFdW_num = g.dFdX(y, W);
  ASSERT(dFdW_num.rows() == W.value().rows())
  ASSERT(dFdW_num.cols() == W.value().cols())

  // dFdW_hat = x
  Tensor dFdW_hat(OUT, IN);
  dFdW_hat << 1, 2,
              1, 2,
              1, 2;
  ASSERT(dFdW_num.isApprox(dFdW_hat, 0.01))

  // dFdW
  ASSERT(dFdW == dFdW_hat)

  // dFdb num
  Tensor dFdb_num = g.dFdX(y, b);
  ASSERT(dFdb_num.rows() == b.value().rows())
  ASSERT(dFdb_num.cols() == b.value().cols())

  // dFdb_hat = 1
  Tensor dFdb_hat = Tensor::Ones(1, OUT);
  ASSERT(dFdb_num.isApprox(dFdb_hat, 0.01))

  // dFdb
  ASSERT(dFdb == dFdb_hat)

  TEST_END()
}

void test_broadcast_forward()
{
  TEST_BEGIN("Broadcast Forward")
  Graph g;

  // size
  int IN = 2;

  // x vector
  Constant x(g, IN, 1);
  x.value() << 1, 2;
  ASSERT(x.forward().rows() == IN);
  ASSERT(x.forward().cols() == 1);

  // y scalar
  Constant s(g, 1,1);
  s.value() << 3.3;
  Broadcast y(g, s, x);
  ASSERT(y.forward().rows() == IN);
  ASSERT(y.forward().cols() == 1);

  // y_hat
  Tensor y_hat(IN, 1);
  y_hat << 3.3, 3.3;
  ASSERT(y_hat == y.forward());

  // z vector
  Variable z(g, IN, 1);
  z.value() << 3,4;
  ASSERT(z.forward().rows() == IN);
  ASSERT(z.forward().cols() == 1);

  // v matrix
  Variable v(g, IN, IN);
  v.value() << 1,2,
               3,4;
  ASSERT(v.forward().rows() == IN);
  ASSERT(v.forward().cols() == IN);

  // b broadcast
  Broadcast b(g, z, v);
  ASSERT(b.forward().rows() == IN);
  ASSERT(b.forward().cols() == IN);

  Function& vb = v + b;

  // vs_hat
  Tensor vb_hat(IN, IN);
  vb_hat << 1+3, 2+3,
            3+4, 4+4;
  ASSERT(vb_hat == vb.forward());

  TEST_END()
}

void test_broadcast_backward()
{
  TEST_BEGIN("Broadcast Backward")
  Graph g;

  // size
  int ROWS = 2;
  int COLS = 3;

  // x vector
  auto& x = *g.new_variable(1, COLS);
  x.value() << 1, 2, 3;
  ASSERT(x.forward().rows() == 1);
  ASSERT(x.forward().cols() == COLS);

  // v matrix
  auto& v = *g.new_variable(ROWS, COLS);
  v.value() << 4,5,6,
               7,8,9;
  ASSERT(v.forward().rows() == ROWS);
  ASSERT(v.forward().cols() == COLS);

  // s broadcast
  auto& s = *g.new_broadcast(x, v);
  ASSERT(s.forward().rows() == ROWS);
  ASSERT(s.forward().cols() == COLS);

  auto& F = v * s;

  // vs_hat
  Tensor F_hat(ROWS, COLS);
  F_hat << 4*1,5*2,6*3,
           7*1,8*2,9*3;
  ASSERT(F_hat == F.forward());

  g.backward(F, Tensor::Ones(ROWS,COLS));

  // dFdv
  Tensor dFdv = v.gradient();
  ASSERT(dFdv.rows() == ROWS);
  ASSERT(dFdv.cols() == COLS);
  ASSERT(dFdv == s.forward());

  // dFds
  Tensor dFds = s.gradient();
  ASSERT(dFds.rows() == ROWS);
  ASSERT(dFds.cols() == COLS);
  ASSERT(dFds == v.forward());

  // dFdx_num
  auto dFdx_num = g.dFdX(F, x);

  // dFdz
  Tensor dFdx = x.gradient();
  ASSERT(dFdx.rows() == 1);
  ASSERT(dFdx.cols() == COLS);
  ASSERT(dFdx.isApprox(dFdx_num, 0.001));

  TEST_END()
}

void test_split_forward()
{
  TEST_BEGIN("Split Forward")

  int IN = 10;
  int OUT = 5;
  srand(time(NULL));
  Graph g;

  Constant x(g, IN, IN);
  x.value() = Tensor::Random(IN, IN);

  Split b(g, x, 2,2, 5,5);
  ASSERT(b() == x().block(2,2, 5,5))

  TEST_END()
}

void test_split_backward()
{
  TEST_BEGIN("Split Backward")

  int IN = 10;
  srand(time(NULL));
  Graph g;

  auto& x = *g.new_variable(IN, IN);
  x.value() = Tensor::Random(IN, IN);

  auto& b = *g.new_split(x, 2,2, 5,5);
  b.forward();

  auto d = Tensor::Ones(5,5);
  g.backward(b, d);
  auto& dFdx = x.gradient();

  Tensor dFdx_hat = Tensor::Zero(IN, IN);
  dFdx_hat.block(2,2, 5,5) = d;

  ASSERT(dFdx == dFdx_hat)
  ASSERT(dFdx.block(2,2, 5,5) == d)

  Tensor dFdx_num = g.dFdX(b, x);
  ASSERT(dFdx.isApprox(dFdx_num, 0.001))

  TEST_END()
}

void test_join_forward()
{
  TEST_BEGIN("Join Forward")

  int IN = 10;
  srand(time(NULL));
  Graph g;

  Constant x(g, IN, IN);
  x.value() = Tensor::Random(IN, IN);

  Split a(g, x, 0,0, 5,IN);
  ASSERT(a() == x().block(0,0, 5,IN))

  Split b(g, x, 5,0, 5,IN);
  ASSERT(b() == x().block(5,0, 5,IN))

  Join y(g, a, b, IN, IN);
  ASSERT(x() == y())

  Constant x2(g, IN, IN);
  x2.value() = Tensor::Constant(IN, IN, 3);

  Constant x3(g, 1, IN);
  x3.value() = Tensor::Constant(1, IN, 4);

  Join x23(g, x2, x3, 1, IN * IN + IN);
  Sum s(g, x23);
  auto s_hat = Tensor::Constant(1, 1, 3 * IN * IN + 4 * IN);
  ASSERT(s() == s_hat)

  TEST_END()
}

void test_join_backward()
{
  TEST_BEGIN("Join Backward")

  int IN = 10;
  srand(time(NULL));
  Graph g;

  auto& x = *g.new_variable(IN, IN);
  x.value() = Tensor::Random(IN, IN);

  auto& a = *g.new_split(x, 0,0, 5,IN);
  auto& b = *g.new_split(x, 5,0, 5,IN);
  auto& y = *g.new_join(a,b, IN,IN);
  y.forward();

  auto d = Tensor::Ones(IN, IN);
  y.gradient() = d;
  auto& dFdx = x.backward();
  ASSERT(dFdx == d)

  Tensor dFdx_num = g.dFdX(y, x);
  ASSERT(dFdx.isApprox(dFdx_num, 0.001))

  auto& x2 = *g.new_variable(IN, IN);
  x2.value() = Tensor::Constant(IN, IN, 3);

  auto& x3 = *g.new_variable(1, IN);
  x3.value() = Tensor::Constant(1, IN, 4);

  auto& x23 = *g.new_join(x2, x3, 1, IN * IN + IN);
  auto& s = *g.new_sum(x23);
  auto sd = Tensor::Ones(1, 1);

  s.forward();
  s.gradient() = sd;

  auto& dFdx2 = x2.backward();
  auto& dFdx3 = x3.backward();

  auto dFdx2_num = g.dFdX(s, x2);
  auto dFdx3_num = g.dFdX(s, x3);

  ASSERT(dFdx2.isApprox(dFdx2_num, 0.01))
  ASSERT(dFdx3.isApprox(dFdx3_num, 0.01))

  TEST_END()
}

void test_min_forward()
{
  TEST_BEGIN("Min Forward")

  int IN = 4;
  Graph g;

  Variable x(g, IN, 1);
  x.value() << -10, -200, 200, 10;

  Constant zero(g, IN, 1);
  zero.value() = Tensor::Zero(IN, 1);

  auto& min = *g.new_min(100 - x, zero);

  Tensor min_hat(IN, 1);
  min_hat << 0, 0, -100, 0;

  ASSERT(min() == min_hat)

  TEST_END()
}

void test_min_backward()
{
  TEST_BEGIN("Min Backward")

  int IN = 4;
  Graph g;

  Variable x(g, IN, 1);
  x.value() << -10, -200, 200, 10;

  Constant zero(g, IN, 1);
  zero.value() = Tensor::Zero(IN, 1);

  auto& min = *g.new_min(100 - x, zero);

  auto dmin_dx_num = g.dFdX(min, x);
  Tensor dmin_dx_hat(IN, 1);
  dmin_dx_hat << 0, 0, -1, 0;

  ASSERT(dmin_dx_num.isApprox(dmin_dx_hat, 0.01))

  min.gradient() = Tensor::Ones(IN, 1);
  auto dmin_dx = x.backward();

  ASSERT(dmin_dx == dmin_dx_hat)

  TEST_END()
}

void test_max_forward()
{
  TEST_BEGIN("Max Forward")

  int IN = 4;
  Graph g;

  Variable x(g, IN, 1);
  x.value() << -10, -200, 200, 10;

  Constant zero(g, IN, 1);
  zero.value() = Tensor::Zero(IN, 1);

  auto& max = *g.new_max(100 - x, zero);

  Tensor max_hat(IN, 1);
  max_hat << 110, 300, 0, 90;

  ASSERT(max() == max_hat)

  TEST_END()
}

void test_max_backward()
{
  TEST_BEGIN("Max Backward")

  int IN = 4;
  Graph g;

  Variable x(g, IN, 1);
  x.value() << -10, -200, 200, 10;

  Constant zero(g, IN, 1);
  zero.value() = Tensor::Zero(IN, 1);

  auto& max = *g.new_max(100 - x, zero);

  auto dmax_dx_num = g.dFdX(max, x);
  Tensor dmax_dx_hat(IN, 1);
  dmax_dx_hat << -1, -1, 0, -1;

  ASSERT(dmax_dx_num.isApprox(dmax_dx_hat, 0.01))

  max.gradient() = Tensor::Ones(IN, 1);
  auto dmax_dx = x.backward();

  ASSERT(dmax_dx == dmax_dx_hat)

  TEST_END()
}

void test_reshape_forward()
{
  TEST_BEGIN("Reshape Forward")

  Graph g;

  auto x = g.new_variable(3,4);
  x->value() << 1, 2, 3, 4,
                5, 6, 7, 8,
                9, 10,11,12;

  Reshape R(g, *x, 2, 6);

  Tensor y_hat(2, 6);
  y_hat << 1, 2, 3, 4, 5, 6,
           7, 8, 9, 10,11,12;

  ASSERT(y_hat == R())

  TEST_END()
}

void test_reshape_backward()
{
  TEST_BEGIN("Reshape Backward")

  Graph g;

  auto x = g.new_variable(3,4);
  x->value() << 1, 2, 3, 4,
                5, 6, 7, 8,
                9, 10,11,12;

  auto R = g.new_reshape(*x, 2, 6);
  R->forward();
  R->gradient() = Tensor::Ones(2, 6);

  Tensor dRdx = x->backward();
  Tensor dRdx_hat = Tensor::Ones(3, 4);
  Tensor dRdx_num = g.dFdX(*R, *x);

  ASSERT(dRdx == dRdx_hat)
  ASSERT(dRdx.isApprox(dRdx_num, 0.001))

  TEST_END()
}

void test_linear_forward()
{
  TEST_BEGIN("Linear Forward")
  Graph g;

  // size
  int X_ROWS = 2;
  int X_COLS = 3;
  int Y_ROWS = 4;
  int Y_COLS = X_COLS;

  // x
  Constant x(g, X_ROWS, X_COLS);
  x.value() <<
    1, 2, 3,
    4, 5, 6;
  ASSERT(x.forward().rows() == X_ROWS);
  ASSERT(x.forward().cols() == X_COLS);

  // y = xW.T + b
  Linear y(g, x, X_COLS, Y_ROWS);

  auto& W = y.W();
  auto& b = y.b();

  ASSERT(W().rows() == Y_ROWS);
  ASSERT(W().cols() == Y_COLS);
  ASSERT(b().rows() == 1);
  ASSERT(b().cols() == Y_ROWS);

  // W
  W.value() <<
     0.5210, -0.3797,  0.2674,
    -0.5357, -0.1399,  0.0647,
     0.3203,  0.0407, -0.3343,
     0.2107, -0.1692,  0.5243;

  // b
  b.value() <<
    0.3992,  0.3767,  0.5552, -0.2610;

  // y
  ASSERT(y().rows() == X_ROWS);
  ASSERT(y().cols() == Y_ROWS);

  // y_hat
  Tensor y_hat(X_ROWS, Y_ROWS);
  y_hat <<
    0.9630, -0.2447, -0.0460,  1.1842,
    2.1891, -2.0774,  0.0341,  2.8816;

  ASSERT(y().isApprox(y_hat, 0.0001))

  TEST_END()
}

void test_linear_backward()
{
  TEST_BEGIN("Linear Backward")
  // size
  int X_ROWS = 2;
  int X_COLS = 3;
  int Y_ROWS = 4;
  int Y_COLS = X_COLS;
  Graph g;

  // x
  Variable& x = *g.new_variable(X_ROWS, X_COLS);
  x.value() <<
    1, 2, 3,
    4, 5, 6;

  // y = xW.T + b
  Linear& y = *g.new_linear(x, X_COLS, Y_ROWS);

  auto& W = y.W();
  auto& b = y.b();
  
  // W
  W.value() <<
     0.5210, -0.3797,  0.2674,
    -0.5357, -0.1399,  0.0647,
     0.3203,  0.0407, -0.3343,
     0.2107, -0.1692,  0.5243;

  // b
  b.value() <<
    0.3992,  0.3767,  0.5552, -0.2610;

  y.forward();
  g.backward(y, Tensor::Ones(X_ROWS, Y_ROWS));

  // dFdW
  Tensor dFdW = W.gradient();
  ASSERT(dFdW.rows() == Y_ROWS);
  ASSERT(dFdW.cols() == Y_COLS);

  // dFdW_num = x
  Tensor dFdW_num = g.dFdX(y, W);
  ASSERT(dFdW_num.rows() == Y_ROWS);
  ASSERT(dFdW_num.cols() == Y_COLS);
  ASSERT(dFdW.isApprox(dFdW_num, 0.01))

  // dFdb
  Tensor dFdb = b.gradient();
  ASSERT(dFdb.rows() == 1);
  ASSERT(dFdb.cols() == Y_ROWS);

  // dFdb_hat = 1
  Tensor dFdb_num = g.dFdX(y, b);
  ASSERT(dFdb_num.rows() == 1);
  ASSERT(dFdb_num.cols() == Y_ROWS);
  ASSERT(dFdb.isApprox(dFdb_num, 0.01))

  // dFdx
  Tensor dFdx = x.gradient();
  ASSERT(dFdx.rows() == X_ROWS);
  ASSERT(dFdx.cols() == X_COLS);

  // dFdx_num = W
  Tensor dFdx_num = g.dFdX(y, x);
  ASSERT(dFdx_num.rows() == X_ROWS);
  ASSERT(dFdx_num.cols() == X_COLS);
  ASSERT(dFdx.isApprox(dFdx_num, 0.01))

  TEST_END()
}

void test_product_forward()
{
  TEST_BEGIN("Product Forward")
  Graph g;

  // size
  int IN = 2;
  int MID = 3;
  int OUT = 4;

  // A matrix
  Variable A(g, OUT, MID);
  A.value() << 1, 2, 3,
               4, 5, 6,
               7, 8, 9,
               10, 11, 12;
  ASSERT(A.forward().rows() == OUT);
  ASSERT(A.forward().cols() == MID);

  // B matrix
  Constant B(g, MID, IN);
  B.value() << 1, 2,
               3, 4,
               5, 6;
  ASSERT(B.forward().rows() == MID);
  ASSERT(B.forward().cols() == IN);

  // y = AB (matrix * matrix)
  Product y(g, A, B);
  ASSERT(y.forward().rows() == OUT);
  ASSERT(y.forward().cols() == IN);

  // y_hat
  Tensor y_hat(OUT, IN);
  y_hat << 22, 28,
           49, 64,
           76, 100,
           103, 136;
  ASSERT(y.forward() == y_hat)

  // U matrix
  Constant U(g, OUT, OUT);
  U.value() <<  3,  -2,   1,  -3,
                6,   5,  -4,   2,
                9,  -8,   7,  -2,
                -9,  11, -10,  -3;

  // h vector
  Constant h(g, OUT, 1);
  h.value() << 0.00293178, -0.0170642, 0.00199824, -0.000237697;

  // Uh = U * h (matrix * vector)
  Product Uh(g, U, h);
  Tensor Uh_hat(OUT, 1);
  Uh_hat << 0.0456, -0.0762,  0.1774, -0.2334;
  ASSERT(Uh().isApprox(Uh_hat, 0.001))

  TEST_END()
}

void test_product_backward()
{
  TEST_BEGIN("Product Backward")

  // size
  int IN = 2;
  int MID = 3;
  int OUT = 4;
  Graph g;

  // A
  auto& A = *g.new_variable(OUT, MID);
  A.value() << 1, 2, 3,
               4, 5, 6,
               7, 8, 9,
               10, 11, 12;

  // B
  auto& B = *g.new_variable(MID, IN);
  B.value() << 1, 2,
               3, 4,
               5, 6;

  // y = AB
  auto& y = *g.new_product(A, B);
  y.forward();
  g.backward(y, Tensor::Ones(OUT,IN));

  // dFdA
  Tensor dFdA = A.gradient();
  ASSERT(dFdA.rows() == OUT);
  ASSERT(dFdA.cols() == MID);

  // dFdA_hat = B
  Tensor dFdA_hat = g.dFdX(y, A);
  ASSERT(dFdA_hat.rows() == OUT);
  ASSERT(dFdA_hat.cols() == MID);
  ASSERT(dFdA.isApprox(dFdA_hat, 0.01))

  // dFdB
  Tensor dFdB = B.gradient();
  ASSERT(dFdB.rows() == MID);
  ASSERT(dFdB.cols() == IN);

  // dFdB_hat = A
  Tensor dFdB_hat = g.dFdX(y, B);
  ASSERT(dFdB_hat.rows() == MID);
  ASSERT(dFdB_hat.cols() == IN);
  ASSERT(dFdB.isApprox(dFdB_hat, 0.01))

  TEST_END()
}

void test_add_forward()
{
  TEST_BEGIN("Add Forward")
  Graph g;

  // size
  int W = 3;
  int H = 2;

  // input A
  Constant A(g, H, W);
  A.value() << 1, 2, 3,
               4, 5, 6;

  // input B
  Constant B(g, H, W);
  B.value() << 7, 8, 9,
               10, 11, 12;

  // x + y
  Tensor y_hat(H, W);
  y_hat <<  8, 10, 12,
            14, 16, 18;

  Add y(g, A, B);
  ASSERT(y.forward() == y_hat)

  TEST_END()
}

void test_add_backward()
{
  TEST_BEGIN("Add Backward")

  // size
  int W = 2;
  int H = 3;
  Graph g;

  // input A
  auto& A = *g.new_variable(H, W);
  A.value() << 1, 2, 
                3, 4,
                5, 6;

  // input B
  auto& B = *g.new_variable(H, W);
  B.value() << 7, 8, 
                9, 10, 
                11, 12;

  // F
  auto& y = *g.new_add(A, B);
  y.forward();
  g.backward(y, Tensor::Ones(H, W));

  // dFdA
  Tensor dFdA = A.gradient();
  ASSERT(dFdA.rows() == H);
  ASSERT(dFdA.cols() == W);

  // dFdB_hat = x
  Tensor dFdA_hat = g.dFdX(y, A);
  ASSERT(dFdA.isApprox(dFdA_hat, 0.01))

  // dFdB
  Tensor dFdB = B.gradient();
  ASSERT(dFdB.rows() == H);
  ASSERT(dFdB.cols() == W);

  // dFdB_hat = x
  Tensor dFdB_hat = g.dFdX(y, B);
  ASSERT(dFdB.isApprox(dFdB_hat, 0.01))

  TEST_END()
}

void test_sub_forward()
{
  TEST_BEGIN("Sub Forward")
  Graph g;

  // size
  int W = 3;
  int H = 2;

  // input A
  Constant A(g, H, W);
  A.value() << 1, 2, 3,
               4, 5, 6;

  // input B
  Constant B(g, H, W);
  B.value() << 7, 8, 9,
               10, 11, 12;

  // x + y
  Tensor y_hat(H, W);
  y_hat <<  -6, -6, -6,
            -6, -6, -6;

  Sub y(g, A, B);
  ASSERT(y.forward() == y_hat)

  TEST_END()
}

void test_sub_backward()
{
  TEST_BEGIN("Sub Backward")

  // size
  int W = 2;
  int H = 3;
  Graph g;

  // input A
  auto& A = *g.new_variable(H, W);
  A.value() << 1, 2, 
                3, 4,
                5, 6;

  // input B
  auto& B = *g.new_variable(H, W);
  B.value() << 7, 8, 
                9, 10, 
                11, 12;

  // F
  auto& y = *g.new_sub(A, B);
  y.forward();
  g.backward(y, Tensor::Ones(H, W));

  // dFdA
  Tensor dFdA = A.gradient();
  ASSERT(dFdA.rows() == H);
  ASSERT(dFdA.cols() == W);

  // dFdB_hat = x
  Tensor dFdA_hat = g.dFdX(y, A);
  ASSERT(dFdA.isApprox(dFdA_hat, 0.01))

  // dFdB
  Tensor dFdB = B.gradient();
  ASSERT(dFdB.rows() == H);
  ASSERT(dFdB.cols() == W);

  // dFdB_hat = x
  Tensor dFdB_hat = g.dFdX(y, B);
  ASSERT(dFdB.isApprox(dFdB_hat, 0.01))

  TEST_END()
}

void test_mul_forward()
{
  TEST_BEGIN("Mul Forward")
  Graph g;

  // size
  int W = 3;
  int H = 2;

  // input A
  Constant A(g, H, W);
  A.value() << 1, 2, 3,
               4, 5, 6;

  // input B
  Constant B(g, H, W);
  B.value() << 7, 8, 9,
               10, 11, 12;

  // x + y
  Tensor y_hat(H, W);
  y_hat <<  7, 16, 27,
            40, 55, 72;

  Mul y(g, A, B);
  ASSERT(y.forward() == y_hat)

  TEST_END()
}

void test_mul_backward()
{
  TEST_BEGIN("Mul Backward")
  Graph g;

  // size
  int W = 2;
  int H = 3;

  // input A
  auto& A = *g.new_variable(H, W);
  A.value() << 1, 2, 
                3, 4,
                5, 6;

  // input B
  auto& B = *g.new_variable(H, W);
  B.value() << 7, 8, 
                9, 10, 
                11, 12;

  // F
  auto& y = *g.new_mul(A, B);
  y.forward();
  g.backward(y, Tensor::Ones(H, W));

  // dFdA
  Tensor dFdA = A.gradient();
  ASSERT(dFdA.rows() == H);
  ASSERT(dFdA.cols() == W);

  // dFdA_hat = B
  Tensor dFdA_hat = g.dFdX(y, A);
  ASSERT(dFdA.isApprox(dFdA_hat, 0.01))
  ASSERT(dFdA == B.forward())

  // dFdB
  Tensor dFdB = B.gradient();
  ASSERT(dFdB.rows() == H);
  ASSERT(dFdB.cols() == W);

  // dFdB_hat = A
  Tensor dFdB_hat = g.dFdX(y, B);
  ASSERT(dFdB.isApprox(dFdB_hat, 0.01))
  ASSERT(dFdB == A.forward())

  TEST_END()
}

void test_power_forward()
{
  TEST_BEGIN("Power Forward")
  Graph g;

  // size
  int W = 3;
  int H = 2;

  // input A
  Constant A(g, H, W);
  A.value() << 1, 2, 3,
               4, 5, 6;

  // input B
  Constant B(g, H, W);
  B.value() << -0.2, 0.3, -0.4,
               2, -3, 4;

  // pow(A,B)
  Tensor y_hat = Eigen::pow(A.value().array(), B.value().array());

  Power y(g, A, B);
  ASSERT(y.forward() == y_hat)

  TEST_END()
}

void test_power_backward()
{
  TEST_BEGIN("Power Backward")
  Graph g;

  // size
  int W = 2;
  int H = 3;

  // input A
  auto& A = *g.new_variable(H, W);
  A.value() << 1, 2, 3,
               4, 5, 6;

  // input B
  auto& B = *g.new_variable(H, W);
  B.value() << -0.2, 0.3, -0.4,
               2, -3, 4;

  // F
  auto& y = *g.new_power(A, B);
  y.forward();
  g.backward(y, Tensor::Ones(H, W));

  // dFdA
  Tensor dFdA = A.gradient();
  ASSERT(dFdA.rows() == H);
  ASSERT(dFdA.cols() == W);

  // dFdA_hat
  Tensor dFdA_hat = g.dFdX(y, A);
  ASSERT(dFdA.isApprox(dFdA_hat, 0.001))

  // dFdB
  Tensor dFdB = B.gradient();
  ASSERT(dFdB.rows() == H);
  ASSERT(dFdB.cols() == W);

  // dFdB_hat
  Tensor dFdB_hat = g.dFdX(y, B);
  ASSERT(dFdB.isApprox(dFdB_hat, 0.001))

  TEST_END()
}

void test_tanh_forward()
{
  TEST_BEGIN("Tanh Forward")
  Graph g;

  // size
  int N = 5;

  // input
  Variable z(g, N, 1);
  z.value() << -2,
                -1, 
                0, 
                1,
                2;

  // answer
  Tensor y_hat(N, 1);
  y_hat << -0.96402758,
           -0.76159416,  
           0,
           0.76159416,
           0.96402758;

  // output
  Tanh y(g, z);
  ASSERT(y.forward().isApprox(y_hat, FINITE_DELTA))

  TEST_END()
}

void test_tanh_backward()
{
  TEST_BEGIN("Tanh Backward")

  // size
  int N = 5;
  Graph g;

  // input
  auto& z = *g.new_variable(N, 2);
  z.value() << -2, -5,
                -1, -3,
                 0,  -2,
                 1,  1,
                 2,  2;

  // gradient
  auto& y = *g.new_tanh(z);
  y.forward();
  g.backward(y, Tensor::Ones(N, 2));

  // dFdz_hat
  Tensor dFdz_hat = g.dFdX(y, z);

  // dFdz
  auto& dFdz = z.gradient();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 2);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.01))

  TEST_END()
}

void test_sigmoid_forward()
{
  TEST_BEGIN("Sigmoid Forward")
  Graph g;

  // size
  int N = 4;

  // sigmoid input
  Constant x(g, N, 1);
  x.value() << 1, 
               0, 
               -3, 
               4;

  // sigmoid output
  Tensor y_hat(N, 1);
  y_hat <<  1 / (1 + exp(-1)), 
            1 / (1 + exp(0)), 
            1 / (1 + exp(3)),  
            1 / (1 + exp(-4));

  Sigmoid y(g, x);
  ASSERT(y.forward().isApprox(y_hat, FINITE_DELTA))
  ASSERT(abs(y.forward().sum()) < N)

  TEST_END()
}

void test_sigmoid_backward()
{
  TEST_BEGIN("Sigmoid Backward")

  // size
  int N = 4;
  Graph g;

  // sigmoid input
  auto& z = *g.new_variable(N, 1);
  z.value() << -1,
                 0, 
                 -3, 
                 4;

  // gradient
  auto& y = *g.new_sigmoid(z);
  y.forward();
  y.gradient() = Tensor::Ones(N, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.01))

  TEST_END()
}

void test_relu_forward()
{
  TEST_BEGIN("ReLU Forward")
  Graph g;

  // size
  int N = 4;

  // relu input
  Constant x(g, N, 1);
  x.value() << 1,
               0,
               -3,
               4;

  // relu output
  Tensor y_hat(N, 1);
  y_hat <<  1,
            0,
            0,
            4;

  ReLU y(g, x);
  ASSERT(y() == y_hat)

  TEST_END()
}

void test_relu_backward()
{
  TEST_BEGIN("ReLU Backward")

  // size
  int N = 4;
  Graph g;

  // relu input
  auto& z = *g.new_variable(N, 1);
  z.value() << -1,
               0,
               -3,
               4;

  // gradient
  auto& y = *g.new_relu(z);
  y.forward();
  y.gradient() = Tensor::Ones(N, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat(N, 1);
  dFdz_hat << 0,
              0,
              0,
              1;
  ASSERT(dFdz_hat == dFdz)

  TEST_END()
}

void test_erf_forward()
{
  TEST_BEGIN("Erf Forward")
  Graph g;

  // size
  int N = 4;

  // relu input
  Constant x(g, N, 1);
  x.value() << 1,
               0,
               -3,
               4;

  // relu output
  Tensor y_hat(N, 1);
  y_hat <<  0.84270079295,
            0,
            -0.9999779095,
            0.99999998458;

  Erf y(g, x);
  ASSERT(y().isApprox(y_hat, 1e-6));

  TEST_END()
}

void test_erf_backward()
{
  TEST_BEGIN("Erf Backward")

  // size
  int N = 4;
  Graph g;

  // relu input
  auto& z = *g.new_variable(N, 1);
  z.value() << -1,
               0,
               -3,
               4;

  // gradient
  auto& y = *g.new_erf(z);
  y.forward();
  y.gradient() = Tensor::Ones(N, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz_hat.isApprox(dFdz, 0.001))

  TEST_END()
}

void test_gelu_forward()
{
  TEST_BEGIN("GeLU Forward")
  Graph g;

  // size
  int N = 7;

  // relu input
  Constant x(g, N, 1);
  x.value() << -2,
               -1,
               -0.5,
               0,
               0.5,
               -3,
               4;

  GeLU y(g, x);

  // relu output
  Tensor y_hat(N, 1);
  y_hat <<  -0.04550027847290039,
            -0.15865525603294373,
            -0.1542687714099884,
            0.0,
            0.3457312285900116,
            -0.0040496885776519775,
            3.999873161315918;

  ASSERT(y_hat.isApprox(y(), 0.01))

  TEST_END()
}

void test_gelu_backward()
{
  TEST_BEGIN("GeLU Backward")

  // size
  int N = 7;
  Graph g;

  // relu input
  auto& z = *g.new_variable(N, 1);
  z.value() << -2,
               -1,
               -0.5,
               0,
               0.5,
               -3,
               4;

  // gradient
  auto& y = *g.new_gelu(z);
  y.forward();
  y.gradient() = Tensor::Ones(N, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat based on Torch GELU
  //
  // x = torch.tensor(float(value), requires_grad=True)
  // y = nn.GELU()(x)
  // y.backward()
  // dFdz_hat = x.grad
  Tensor dFdz_hat(N, 1);
  dFdz_hat << -0.08523179590702057,
              -0.08331547677516937,
              0.1325048804283142,
              0.5,
              0.8674951195716858,
              -0.011945649050176144,
              1.000503659248352;

  ASSERT(dFdz_hat.isApprox(dFdz, 0.03))

  TEST_END()
}

void test_dropout_forward()
{
  TEST_BEGIN("Dropout Forward")
  Graph g;

  // size
  int N = 100;
  int M = 500;

  // dropout rate
  DTYPE R = 0.2;

  // dropout input
  Constant x(g, N, M);
  x.value() = Tensor::Ones(N, M);

  Dropout y(g, x, R);

  // compute the actual dropout rate
  DTYPE ones = y().sum();
  DTYPE rate = (N*M - ones) / (N*M);
  ASSERT(abs(rate - R) < 0.01)

  TEST_END()
}

void test_dropout_backward()
{
  TEST_BEGIN("Dropout Backward")

  // size
  int N = 100;
  int M = 500;

  // dropout rate
  DTYPE R = 0.2;

  Graph g;

  // dropout input
  auto& x = *g.new_variable(N, M);
  x.value() = Tensor::Ones(N, M);

  // gradient
  auto& y = *g.new_dropout(x, R);
  y.forward();
  y.gradient() = Tensor::Ones(N, M);

  // dFdx
  auto& dFdx = x.backward();
  ASSERT(dFdx.rows() == N);
  ASSERT(dFdx.cols() == M);
  ASSERT(y() == dFdx)

  TEST_END()
}

void test_softmax_forward()
{
  TEST_BEGIN("Softmax Forward")
  Graph g;

  // size
  int N = 4;

  // softmax input
  Constant x(g, 1, N);
  x.value() << -1, 
                0, 
                -3, 
                4;

  // softmax([-1,0,-3,4])
  Tensor y_hat(1, N);
  y_hat <<  6.56742084e-03,
            1.78521007e-02,
            8.88803760e-04, 
            9.74691675e-01;

  Softmax y(g, x);
  ASSERT(y.forward().isApprox(y_hat, FINITE_DELTA))
  ASSERT(abs(y.forward().sum() - 1) < FINITE_DELTA)

  y.recache();
  x.value() << 0, 
               0, 
               0, 
               0;

  // softmax([0,0,0,0])
  y_hat <<  0.25,
            0.25,
            0.25, 
            0.25;
  ASSERT(y.forward() == y_hat)
  ASSERT(abs(y.forward().sum() - 1) < FINITE_DELTA)

  TEST_END()
}

void test_softmax_backward()
{
  TEST_BEGIN("Softmax Backward")

  // size
  int N = 4;
  Graph g;

  // softmax input
  auto& z = *g.new_variable(1, N);
  z.value() << 1, 2, 3, 4;

  // F
  auto& y = *g.new_softmax(z);
  ASSERT(y().rows() == 1);
  ASSERT(y().cols() == N);

  Tensor dy = Tensor::Ones(1, N);
  dy << 5;
  y.gradient() = dy;

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == 1);
  ASSERT(dFdz.cols() == N);

  // dFdX_hat
  Tensor dFdz_hat(1, N);
  dFdz_hat << 0.1241, -0.0112, -0.0304, -0.0826;
  ASSERT(dFdz.isApprox(dFdz_hat, 0.01))

  // dFdX_num
  Tensor dFdX_num = g.dFdX(y, z);
  ASSERT(dFdz.isApprox(dFdX_num, 0.01))

  TEST_END()
}

void test_softplus_forward()
{
  TEST_BEGIN("Softplus Forward")
  Graph g;

  // size
  int N = 6;

  // softplus input
  Constant x(g, N, 1);
  x.value() << -100,-1,0,
                -3,4,100;

  // softplus output
  Tensor y_hat(N, 1);
  y_hat <<  0.0, 3.13261688e-01, 6.93147181e-01,
            4.85873516e-02, 4.01814993e+00, 100;

  Softplus y(g, x);
  ASSERT(y().isApprox(y_hat, 0.001))

  TEST_END()
}

void test_softplus_backward()
{
  TEST_BEGIN("Softplus Backward")

  // size
  int N = 6;
  Graph g;

  // softplus input
  auto& z = *g.new_variable(N, 1);
  z.value() << -100,-1,0,
                -3,4,100;

  // F
  auto& y = *g.new_softplus(z);
  y.forward();
  y.gradient() = Tensor::Ones(N, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat(N, 1);
  dFdz_hat << 3.72007598e-44, 2.68941421e-01, 5.00000000e-01,           
              4.74258732e-02, 9.82013790e-01, 1.00000000e+00;
  ASSERT(dFdz.isApprox(dFdz_hat, 0.001))

  dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.001))

  TEST_END()
}

void test_log_softmax_forward()
{
  TEST_BEGIN("Log Softmax Forward")
  Graph g;

  // size
  int N = 4;

  // log softmax input
  Constant x(g, 1, N);
  x.value() << -1, 0, -3, 4;

  // log_softmax([-1,0,-3,4])
  Tensor y_hat(1, N);
  y_hat <<  -5.0256, -4.0256, -7.0256, -0.0256;

  LogSoftmax y(g, x);
  auto F = y.forward();
  ASSERT(y.forward().isApprox(y_hat, 0.01))
  ASSERT(y.forward().rows() == 1)
  ASSERT(y.forward().cols() == N)

  TEST_END()
}

void test_log_softmax_backward()
{
  TEST_BEGIN("Log Softmax Backward")

  // size
  int N = 4;
  Graph g;

  // softmax input
  auto& z = *g.new_variable(1, N);
  z.value() << -1, 0, -3, 4;

  // F
  auto& y = *g.new_log_softmax(z);
  y.forward();
  y.gradient() = Tensor::Ones(1, N);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == 1);
  ASSERT(dFdz.cols() == N);

  // dFdX_hat
  Tensor dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.01))

  TEST_END()
}

void test_log_forward()
{
  TEST_BEGIN("Log Forward")
  Graph g;

  // size
  int N = 4;

  // input
  Variable z(g, N, 1);
  z.value() << 0,
               1, 
               2, 
               3;

  // answer
  Tensor y_hat(N, 1);
  y_hat << -INFINITY,
            0,
            0.69314718,
            1.09861229;

  // output
  Log F(g, z);
  Tensor y = F.forward();
  ASSERT(std::isinf(-y(0)))
  ASSERT(abs(y(1) - y_hat(1)) < FINITE_DELTA)
  ASSERT(abs(y(2) - y_hat(2)) < FINITE_DELTA)
  ASSERT(abs(y(3) - y_hat(3)) < FINITE_DELTA)

  TEST_END()
}

void test_log_backward()
{
  TEST_BEGIN("Log Backward")

  // size
  int N = 4;
  Graph g;

  // input
  auto& z = *g.new_variable(N, 1);
  z.value() << 0,
                1,
                2,
                3;

  // gradient
  auto& y = *g.new_log(z);
  y.forward();
  y.gradient() = Tensor::Ones(N, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat(N, 1);
  dFdz_hat << INFINITY,
              g.dFdX(y, z, 1, 0, 1, 0),
              g.dFdX(y, z, 2, 0, 2, 0),
              g.dFdX(y, z, 3, 0, 3, 0);

  ASSERT(std::isinf(dFdz(0)))
  ASSERT(abs(dFdz(1) - dFdz_hat(1)) < 0.01)
  ASSERT(abs(dFdz(2) - dFdz_hat(2)) < 0.01)
  ASSERT(abs(dFdz(3) - dFdz_hat(3)) < 0.01)

  TEST_END()
}

void test_sum_forward()
{
  TEST_BEGIN("Sum Forward")
  Graph g;

  // size
  int N = 4;

  // input
  Variable z(g, N, 1);
  z.value() << 0,
               1, 
               2, 
               3;

  // answer
  Tensor y_hat(1, 1);
  y_hat << 0 + 
           1 + 
           2 + 
           3;

  // output
  Sum y(g, z);
  ASSERT(y.forward().isApprox(y_hat, FINITE_DELTA))

  TEST_END()
}

void test_sum_backward()
{
  TEST_BEGIN("Sum Backward")

  // size
  int N = 4;
  Graph g;

  // input
  auto& z = *g.new_variable(N, 1);
  z.value() << 0,
               1,
               2,
               3;

  // gradient
  auto& y = *g.new_sum(z);
  y.forward();
  y.gradient() = Tensor::Ones(1, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.01))

  TEST_END()
}

void test_abs_forward()
{
  TEST_BEGIN("Abs Forward")
  Graph g;

  // size
  int N = 5;

  // input
  Variable z(g, N, 1);
  z.value() << -2, -1, 0, 1, 3;

  // answer
  Tensor y_hat(N, 1);
  y_hat << 2, 1, 0, 1, 3;

  // output
  Abs y(g, z);
  ASSERT(y.forward() == y_hat)

  TEST_END()
}

void test_abs_backward()
{
  TEST_BEGIN("Abs Backward")

  // size
  int N = 5;
  Graph g;

  // input
  auto& z = *g.new_variable(N, 1);
  z.value() << -2, -1, 0, 1, 3;

  // gradient
  auto& y = *g.new_abs(z);
  y.forward();
  y.gradient() = Tensor::Ones(N, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.01))

  TEST_END()
}

void test_transpose_forward()
{
  TEST_BEGIN("Transpose Forward")
  Graph g;

  // size
  int N = 5;
  int M = 2;

  // input
  Variable z(g, M, N);
  z.value() << -2, -1, 0, 1, 3,
               22, 11, 5, 2, 1;

  // answer
  Tensor y_hat(N, M);
  y_hat << -2, 22,
           -1, 11,
            0, 5,
            1, 2,
            3, 1;

  // output
  Transpose y(g, z);
  ASSERT(y().rows() == N);
  ASSERT(y().cols() == M);
  ASSERT(y.forward() == y_hat)

  TEST_END()
}

void test_transpose_backward()
{
  TEST_BEGIN("Transpose Backward")

  // size
  int N = 5;
  int M = 2;
  Graph g;

  // input
  auto& z = *g.new_variable(M, N);
  z.value() << -2, -1, 0, 1, 3,
               22, 11, 5, 2, 1;

  // gradient
  auto& y = *g.new_transpose(z);
  auto ones = Tensor::Ones(N, M);
  y.forward();
  y.gradient() = ones;

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == M);
  ASSERT(dFdz.cols() == N);
  ASSERT(dFdz == ones.transpose());

  // dFdX_hat
  Tensor dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz_hat.rows() == M);
  ASSERT(dFdz_hat.cols() == N);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.001))

  TEST_END()
}

void test_mean_forward()
{
  TEST_BEGIN("Mean Forward")
  Graph g;

  // size
  int N = 5;

  // input
  Variable z(g, N, 1);
  z.value() << -2, -1, 0, 1, 3;

  // answer
  Tensor y_hat(1, 1);
  y_hat << (float)(-2 -1 + 0 + 1 + 3) / N;

  // output
  Mean y(g, z);
  ASSERT(y.forward() == y_hat)

  TEST_END()
}

void test_mean_backward()
{
  TEST_BEGIN("Mean Backward")

  // size
  int N = 5;
  Graph g;

  // softmax input
  auto& z = *g.new_variable(N, 1);
  z.value() << -2, -1, 0, 1, 3;

  // gradient
  auto& y = *g.new_mean(z);
  y.forward();
  y.gradient() = Tensor::Ones(1, 1);

  // dFdz
  auto& dFdz = z.backward();
  ASSERT(dFdz.rows() == N);
  ASSERT(dFdz.cols() == 1);

  // dFdX_hat
  Tensor dFdz_hat = g.dFdX(y, z);
  ASSERT(dFdz.isApprox(dFdz_hat, 0.01))

  TEST_END()
}

void test_stack_forward()
{
  TEST_BEGIN("Stack Forward")

  // size
  int IN = 4;
  int MID = 3;
  int OUT = 2;
  Graph g;

  // x1
  auto& x1 = *g.new_variable(1, IN);
  x1.value() << 1, 2, 3, 4;

  // x2 = W1 * x1 + b1
  auto& x2 = *g.new_linear(x1, IN, MID);

  // W1
  auto& W1 = x2.W();
  W1.value() << 1, 2, 3, 4,
                5, 6, 7, 8,
                9, 10, 11, 12;

  // b1
  auto& b1 = x2.b();
  b1.value() << 1, 2, 3;

  ASSERT(x2.forward().rows() == 1)
  ASSERT(x2.forward().cols() == MID)

  // y2 = W2 * x2 + b2
  auto& y2 = *g.new_linear(x2, MID, OUT);

  // W2
  auto& W2 = y2.W();
  W2.value() << 1, 2, 3,
                4, 5, 6;

  // b2
  auto& b2 = y2.b();
  b2.value() << 1, 2;

  ASSERT(y2.forward().rows() == 1)
  ASSERT(y2.forward().cols() == OUT)
  
  // answer y1_hat
  Tensor y1_hat(1, MID);
  y1_hat << 31,  72, 113;
  ASSERT(x2.forward() == y1_hat)
  
  // answer y2_hat
  Tensor y2_hat(1, OUT);
  y2_hat << 515, 1164;
  ASSERT(y2.forward() == y2_hat)

  TEST_END()
}

void test_stack_backward()
{
  TEST_BEGIN("Stack Backward")

  // size
  int IN = 2;
  int MID = 3;
  int OUT = 4;
  Graph g;

  // x1
  auto& x1 = *g.new_variable(1, IN);
  x1.value() << 1, 2;

  // x2 = W1 * x1 + b1
  auto& x2 = *g.new_linear(x1, IN, MID);

  // W1
  auto& W1 = x2.W();
  W1.value() << 1, 2,
                3, 4,
                5, 6;

  // b1
  auto& b1 = x2.b();
  b1.value() << 1, 2, 3;

  // x3 = W2 * x2 + b2
  auto& x3 = *g.new_linear(x2, MID, OUT);

  // W2
  auto& W2 = x3.W();
  W2.value() << 1, 2, 3,
                4, 5, 6,
                7, 8, 9,
                10, 11, 12;

  // b2
  auto& b2 = x3.b();
  b2.value() << 1, 2, 3, 4;
  
  // gradient
  x3.forward();
  x3.gradient() = Tensor::Ones(1, OUT);
  auto dx3dx1 = x1.backward();
  auto dx3dW1 = W1.backward();
  auto dx3db1 = b1.backward();
  auto dx3dW2 = W2.backward();
  auto dx3db2 = b2.backward();

  // dx3dx1_hat
  Tensor dx3dx1_hat = g.dFdX(x3, x1);
  ASSERT(dx3dx1_hat.rows() == 1);
  ASSERT(dx3dx1_hat.cols() == IN);
  ASSERT(dx3dx1.isApprox(dx3dx1_hat, 0.01))

  // dx3dW1_hat
  Tensor dx3dW1_hat = g.dFdX(x3, W1);
  ASSERT(dx3dW1_hat.rows() == MID);
  ASSERT(dx3dW1_hat.cols() == IN);
  ASSERT(dx3dW1.isApprox(dx3dW1_hat, 0.01))

  // dx3db1_hat
  Tensor dx3db1_hat = g.dFdX(x3, b1);
  ASSERT(dx3db1_hat.rows() == 1);
  ASSERT(dx3db1_hat.cols() == MID);
  ASSERT(dx3db1.isApprox(dx3db1_hat, 0.01))
  
  TEST_END()
}

void test_gru_forward()
{
  TEST_BEGIN("GRU Forward")

  // size
  int IN = 3;
  int OUT = 4;
  Graph g;

  auto& x = *g.new_variable(1, IN);
  auto& h = *g.new_variable(1, OUT);
  auto& y = *g.new_gru(x, h, IN, OUT);

  x.value()     << 0.01,    -0.02,    0.03;
  h.value()     << 0.01,    -0.02,    0.03,   -0.03;

  y.Wz().value() <<
    1, -4, 7, -9,
    2, -5, 8, -9,
    3, -6, 7, -9;
  y.Uz().value() <<
    3, -6, 9, -9,
    2, -5, 8, -9,
    1, -4, 7, -9,
    -1, 1, -1, 1;
  y.bz().value() << 1,2,-3,-4;

  ASSERT(y.Wz().value().rows() == IN)
  ASSERT(y.Wz().value().cols() == OUT)
  ASSERT(y.Uz().value().rows() == OUT)
  ASSERT(y.Uz().value().cols() == OUT)
  ASSERT(y.bz().value().rows() == 1)
  ASSERT(y.bz().value().cols() == OUT)

  y.Wr().value() <<
    2, -5, 8, -10,
    2, -5, 8, 10,
    3, -6, 9, -10;
  y.Ur().value() <<
    3, -6, 9, -10,
    2, -5, 8, -10,
    1, -4, 7, -10,
    -1, 1, -1, 1;
  y.br().value() << -1,2,-3,-4;

  ASSERT(y.Wr().value().rows() == IN)
  ASSERT(y.Wr().value().cols() == OUT)
  ASSERT(y.Ur().value().rows() == OUT)
  ASSERT(y.Ur().value().cols() == OUT)
  ASSERT(y.br().value().rows() == 1)
  ASSERT(y.br().value().cols() == OUT)

  y.Wh().value() <<
    -4, -7, -7, 10,
    2, 5, 8, -12,
    3, -6, 5, 10;
  y.Uh().value() <<
    3, 6, 9, -9,
    -2, 5, -8, 11,
    1, -4, 7, -10,
    -3, 2, -2, -3;
  y.bh().value() << -1,2,-3,-4;

  ASSERT(y.Wh().value().rows() == IN)
  ASSERT(y.Wh().value().cols() == OUT)
  ASSERT(y.Uh().value().rows() == OUT)
  ASSERT(y.Uh().value().cols() == OUT)
  ASSERT(y.bh().value().rows() == 1)
  ASSERT(y.bh().value().cols() == OUT)

  Tensor y_hat(1,OUT);
  y_hat << -0.1752,  0.1165, -0.9301, -0.9866;

  ASSERT(y().isApprox(y_hat, 0.001))

  TEST_END()
}

void test_gru_backward()
{
  TEST_BEGIN("GRU Backward")

  // size
  int IN = 3;
  int OUT = 4;
  Graph g;

  auto& x = *g.new_variable(1, IN);
  auto& h = *g.new_variable(1, OUT);
  auto& y = *g.new_gru(x, h, IN, OUT);

  x.value()     << 0.01,    -0.02,    0.03;
  h.value()     << 0.01,    -0.02,    0.03,   -0.03;

  y.Wz().value() <<
    1, -4, 7, -9,
    2, -5, 8, -9,
    3, -6, 7, -9;
  y.Uz().value() <<
    3, -6, 9, -9,
    2, -5, 8, -9,
    1, -4, 7, -9,
    -1, 1, -1, 1;
  y.bz().value() << 1,2,-3,-4;

  y.Wr().value() <<
    2, -5, 8, -10,
    2, -5, 8, 10,
    3, -6, 9, -10;
  y.Ur().value() <<
    3, -6, 9, -10,
    2, -5, 8, -10,
    1, -4, 7, -10,
    -1, 1, -1, 1;
  y.br().value() << -1,2,-3,-4;

  y.Wh().value() <<
    -4, -7, -7, 10,
    2, 5, 8, -12,
    3, -6, 5, 10;
  y.Uh().value() <<
    3, 6, 9, -9,
    -2, 5, -8, 11,
    1, -4, 7, -10,
    -3, 2, -2, -3;
  y.bh().value() << -1,2,-3,-4;

  y.forward();
  y.gradient() = Tensor::Ones(1, OUT);
  auto& dydx = x.backward();
  auto& dydh = h.backward();

  auto dydx_num = g.dFdX(y,x);
  auto dydh_num = g.dFdX(y,h);

  Tensor dydx_hat(1,IN);
  Tensor dydh_hat(1,OUT);
  dydx_hat << 0.2577, 1.6326, 1.7202;
  dydh_hat << 2.4663,  1.9466,  0.9862, -0.2952;

  ASSERT(dydx.isApprox(dydx_hat, 0.001))
  ASSERT(dydh.isApprox(dydh_hat, 0.001))

  ASSERT(dydx.isApprox(dydx_num, 0.001))
  ASSERT(dydh.isApprox(dydh_num, 0.001))

  TEST_END()
}

void test_norm_forward()
{
  TEST_BEGIN("Norm Forward")

  // size
  Graph g;

  auto& x = *g.new_variable(2, 3);

  x.value() <<  1, 2, 3,
                4, 5, 6;

  Tensor y_hat(2, 3);
  y_hat << -1.4638, -0.8783, -0.2928,
            0.2928,  0.8783,  1.4638;

  auto& N = *g.new_norm(x, 2, 3);
  auto& y = N.forward();

  ASSERT(y.isApprox(y_hat, 0.0001))

  TEST_END()
}

void test_norm_backward()
{
  TEST_BEGIN("Norm Backward")

  // size
  Graph g;

  auto& x = *g.new_variable(2,3);
  x.value() <<  1, 2, 3,
                4, 5, 6;

  int ROWS = x.value().rows();
  int COLS = x.value().cols();

  auto& N = *g.new_norm(x, ROWS, COLS);
  auto& A = N.A();
  auto& B = N.B();

  N.forward();
  N.gradient() = Tensor::Zero(ROWS, COLS);
  N.gradient()(0) = 1;

  auto& dNdx = x.backward();
  auto& dNdG = A.backward();
  auto& dNdB = B.backward();

  // partial torch derivative of N(0) wrt x
  Tensor dNdx_hat(ROWS, COLS);
  dNdx_hat << 0.2788, -0.2231, -0.1394,
              -0.0558,  0.0279,  0.1115;
  ASSERT(dNdx.isApprox(dNdx_hat, 0.1))

  // partial numerical derivative of N(0) wrt x
  auto dNdx_num = g.dFdX(N, x);
  ASSERT(dNdx.isApprox(dNdx_num, 0.1))

  // partial numerical derivative of N(0) wrt A
  auto dNdA_num = g.dFdX(N, A);
  ASSERT(dNdG.isApprox(dNdA_num, 0.1))

  // partial numerical derivative of N(0) wrt B
  auto dNdB_num = g.dFdX(N, B);
  ASSERT(dNdB.isApprox(dNdB_num, 0.1))

  TEST_END()
}

void test_gaussian_forward()
{
  TEST_BEGIN("Gaussian Forward")

  // size
  int IN = 5;
  int OUT = 5;
  Graph g;

  auto& x = *g.new_variable(IN,1);
  auto& m = *g.new_variable(IN,1);
  auto& s = *g.new_variable(IN,1);
  auto& y = *g.new_gaussian(x, m, s);

  x.value() << -1.01,   0.0,    2.1,   3.5,   5.0;
  m.value() << -1.0,    0.0,    2.0,   3.0,   4.0;
  s.value() <<  0.01,   0.1,    1.0,   2.0,   3.0;

  Tensor y_hat(OUT,1);
  y_hat     << 24.197072451914313,
               3.989422804014327,
               0.3969525474770118,
               0.19333405840142465,
               0.12579440923099774;

  ASSERT(y().isApprox(y_hat, 0.001))

  TEST_END()
}

void test_gaussian_backward()
{
  TEST_BEGIN("Gaussian Backward")

  // size
  int IN = 5;
  int OUT = 5;
  Graph g;

  auto& x = *g.new_variable(IN,1);
  auto& m = *g.new_variable(IN,1);
  auto& s = *g.new_variable(IN,1);
  auto& y = *g.new_gaussian(x, m, s);

  m.value() << -1.0,    0.0,    2.0,   3.0,   4.0;
  s.value() <<  0.02,   0.1,    1.0,   2.0,   3.0;
  x.value() << -1.05,   0.0,    2.1,   3.5,   5.0;

  y.forward();
  y.gradient() = Tensor::Ones(OUT,1);

  auto& dydx = x.backward();
  auto& dydm = m.backward();
  auto& dyds = s.backward();

  auto dydx_num = g.dFdX(y,x);
  auto dydm_num = g.dFdX(y,m);
  auto dyds_num = g.dFdX(y,s);

  ASSERT(dydx.isApprox(dydx_num, 0.01))
  ASSERT(dydm.isApprox(dydm_num, 0.01))
  ASSERT(dyds.isApprox(dyds_num, 0.01))

  TEST_END()
}

void test_log_gaussian_forward()
{
  TEST_BEGIN("Log Normal Forward")

  // size
  int IN = 5;
  int OUT = 5;
  Graph g;

  auto& x = *g.new_variable(IN,1);
  auto& m = *g.new_variable(IN,1);
  auto& s = *g.new_variable(IN,1);
  auto& y = *g.new_log_gaussian(x, m, s);

  x.value() << -1.01,   0.0,    2.1,   3.5,   5.0;
  m.value() << -1.0,    0.0,    2.0,   3.0,   4.0;
  s.value() <<  0.01,   0.1,    1.0,   2.0,   3.0;

  Tensor y_hat(OUT,1);
  y_hat     << 3.186231652783418,
               1.383646559789373,
               -0.9239385332046727,
               -1.6433357137646178,
               -2.073106377428338;

  ASSERT(y().isApprox(y_hat, 0.001))

  TEST_END()
}

void test_log_gaussian_backward()
{
  TEST_BEGIN("Log Normal Backward")

  // size
  int IN = 5;
  int OUT = 5;
  Graph g;

  auto& x = *g.new_variable(IN,1);
  auto& m = *g.new_variable(IN,1);
  auto& s = *g.new_variable(IN,1);
  auto& y = *g.new_log_gaussian(x, m, s);

  m.value() << -1.0,    0.0,    2.0,   3.0,   4.0;
  s.value() <<  0.02,   0.1,    1.0,   2.0,   3.0;
  x.value() << -1.05,   0.0,    2.1,   3.5,   5.0;

  y.forward();
  y.gradient() = Tensor::Ones(OUT,1);

  auto& dydx = x.backward();
  auto& dydm = m.backward();
  auto& dyds = s.backward();

  auto dydx_num = g.dFdX(y,x);
  auto dydm_num = g.dFdX(y,m);
  auto dyds_num = g.dFdX(y,s);

  ASSERT(dydx.isApprox(dydx_num, 0.01))
  ASSERT(dydm.isApprox(dydm_num, 0.01))
  ASSERT(dyds.isApprox(dyds_num, 0.01))

  TEST_END()
}

void test_embedding_forward()
{
  TEST_BEGIN("Embedding Forward")

  // size
  int IN = 10; // vocabulary size
  int OUT = 5; // embedding size
  Graph g;

  Constant two_hot(g, 2, 1);
  two_hot.value() << 1, 3;

  Embedding E(g, two_hot, IN, OUT);
  auto& W = E.E().value();

  Tensor y_hat(2, OUT);
  y_hat.row(0) = W.row(1);
  y_hat.row(1) = W.row(3);
  ASSERT(E() == y_hat);

  TEST_END()
}

void test_embedding_backward()
{
  TEST_BEGIN("Embedding Backward")

  // size
  int IN = 10; // vocabulary size
  int OUT = 5; // embedding size
  Graph g;

  Constant& two_hot = *g.new_constant( 2, 1);
  two_hot.value() << 2, 3;

  Embedding& E = *g.new_embedding(two_hot, IN, OUT);
  auto& W = E.E();

  E.forward();
  E.gradient() = Tensor::Ones(2, OUT);

  auto& dEdW = W.backward();
  auto dEdW_num = g.dFdX(E, W);
  Tensor dEdW_hat = Tensor::Zero(IN, OUT);
  dEdW_hat.row(2) = Tensor::Ones(1, OUT);
  dEdW_hat.row(3) = Tensor::Ones(1, OUT);

  ASSERT(dEdW.isApprox(dEdW_num, 0.001));
  ASSERT(dEdW == dEdW_hat);

  TEST_END()
}

void test_conv2d_forward()
{
  TEST_BEGIN("Conv2D Forward Single-Channel")

  // size
  int IN_ROWS = 2;
  int IN_COLS = 3;

  int IN_CHANNELS = 1;
  int OUT_CHANNELS = 1;

  int K_ROWS = 2;
  int K_COLS = 2;

  int STRIDE = 1;
  int PADDING = 1;
  int DILATION = 2;

  int OUT_ROWS = 2;
  int OUT_COLS = 3;

  // size
  Graph g;

  // 2D input
  Tensor x2d(IN_ROWS, IN_COLS);
  x2d << 1, 2, 3,
         4, 5, 6;

  // convert 2D input to 1D col-major vector
  Variable x(g, 1, x2d.size());
  x.value() = ConstRowVectorMap(x2d.data(), x2d.size());

  // 2D conv layer
  Conv2D c(
    g, x,
    IN_ROWS, IN_COLS,
    IN_CHANNELS, OUT_CHANNELS,
    K_ROWS, K_COLS,
    STRIDE, PADDING, DILATION
  );

  // K
  auto& K = c.K();
  K.value() << 1, 2,
               3, 4;

  // 2D convolution
  auto y = c.forward();

  // reshape vector output to 2D row-major matrix
  Tensor y2d = ConstTensorMap(y.data(), OUT_ROWS, OUT_COLS);

  // expected output
  Tensor y_hat(OUT_ROWS, OUT_COLS);
  y_hat << 20, 36, 15,
            4,  7,  2;
  
  ASSERT(y_hat == y2d);

  TEST_END()

  TEST_BEGIN("Conv2D Forward Multi-Channel")

  // size
  int IN_ROWS = 2;
  int IN_COLS = 3;

  int IN_CHANNELS = 2;
  int OUT_CHANNELS = 3;

  int K_ROWS = 2;
  int K_COLS = 2;

  int STRIDE = 1;
  int PADDING = 1;
  int DILATION = 2;

  int OUT_ROWS = 2;
  int OUT_COLS = 3;

  // size
  Graph g;

  // 2D input, IN_CHANNELS * IN_ROWS, IN_COLS = 4x3;
  //
  //  1  2  3 // channel 1
  //  4  5  6
  //
  //  7  8  9 // channel 2
  // 10 11 12

  // 2D input as vector
  Variable x(g, 1, IN_CHANNELS * IN_ROWS * IN_COLS);
  // col major
  //x.value() << 1,4, 2,5, 3,6, 7,10, 8,11, 9,12;
  // row major
  x.value() << 1,2,3, 4,5,6, 7,8,9, 10,11,12;

  // 2D conv layer
  Conv2D c(
    g, x,
    IN_ROWS, IN_COLS,
    IN_CHANNELS, OUT_CHANNELS,
    K_ROWS, K_COLS,
    STRIDE, PADDING, DILATION
  );

  // K, OUT_CHANNELS * K_ROWS, IN_CHANNELS * K_COLS = 6x4
  //
  //  1  2    5  6
  //  3  4    7  8
  //
  //  9 10   11 12
  // 13 14   15 16
  //
  // 17 18   19 20
  // 21 22   23 34

  auto& K = c.K();
  K.value() << 1, 2,  5, 6,
               3, 4,  7, 8,
               9,10, 11,12,
              13,14, 15,16,
              17,18, 19,20,
              21,22, 23,24;

  // 2D convolution
  auto y = c.forward();

  Tensor y2d(OUT_CHANNELS * OUT_ROWS, OUT_COLS);
  for (int i=0; i<OUT_CHANNELS; i++)
  {
    auto ch = y.block(0, i * OUT_ROWS * OUT_COLS, 1, OUT_ROWS * OUT_COLS);
    y2d.block(i * OUT_ROWS, 0, OUT_ROWS, OUT_COLS) =         
      ConstTensorMap(ch.data(), OUT_ROWS, OUT_COLS);
  }

  // expected output
  Tensor y_hat(OUT_CHANNELS * OUT_ROWS, OUT_COLS);
  y_hat << 108, 202,  92, // channel 1
            52,  96,  42,
           246, 478, 230, // channel 2
           116, 224, 106,
           374, 734, 358, // channel 3
           196, 384, 186;
  ASSERT(y_hat == y2d);

  TEST_END()
}

void test_conv2d_backward()
{
  TEST_BEGIN("Conv2D Backward Single-Channel")

  // size
  int IN_ROWS = 2;
  int IN_COLS = 3;

  int IN_CHANNELS = 1;
  int OUT_CHANNELS = 1;

  int K_ROWS = 2;
  int K_COLS = 2;

  int STRIDE = 1;
  int PADDING = 1;
  int DILATION = 1;

  int OUT_ROWS = 3;
  int OUT_COLS = 4;

  int IN = IN_ROWS * IN_COLS;
  int OUT = OUT_ROWS * OUT_COLS;

  Graph g;

  // matrix input
  Tensor x2d(IN_ROWS, IN_COLS);
  x2d << 1, 2, 3,
         4, 5, 6;

  // vector input
  auto& x = *g.new_variable(1, IN);
  x.value() = ConstTensorMap(x2d.data(), 1, x2d.size());

  // 2D convolution
  auto& y = *g.new_conv2d(
    x,
    IN_ROWS, IN_COLS,
    IN_CHANNELS, OUT_CHANNELS,
    K_ROWS, K_COLS,
    STRIDE, PADDING, DILATION
  );

  // K
  auto& K = y.K();
  K.value() << 1, 2,
               3, 4;

  y.forward();
  g.backward(y, Tensor::Ones(1, OUT));

  // dFdK
  Tensor dFdK = K.gradient();
  ASSERT(dFdK.rows() == K_ROWS);
  ASSERT(dFdK.cols() == K_COLS);

  // dFdx
  Tensor dFdx = x.gradient();
  ASSERT(dFdx.rows() == 1);
  ASSERT(dFdx.cols() == IN);

  // dFdx_hat = K
  Tensor dFdx_hat = g.dFdX(y, x);
  ASSERT(dFdx_hat.rows() == 1);
  ASSERT(dFdx_hat.cols() == IN);
  ASSERT(dFdx.isApprox(dFdx_hat, 0.01))
  
  // dFdK_hat = x
  Tensor dFdK_hat = g.dFdX(y, K);
  ASSERT(dFdK_hat.rows() == K_ROWS);
  ASSERT(dFdK_hat.cols() == K_COLS);
  ASSERT(dFdK.isApprox(dFdK_hat, 0.01))
  
  TEST_END()

  TEST_BEGIN("Conv2D Backward Multi-Channel")

  // size
  int IN_ROWS = 2;
  int IN_COLS = 3;

  int IN_CHANNELS = 2;
  int OUT_CHANNELS = 3;

  int K_ROWS = 2;
  int K_COLS = 2;

  int STRIDE = 1;
  int PADDING = 1;
  int DILATION = 2;

  int OUT_ROWS = 2;
  int OUT_COLS = 3;

  int IN = IN_CHANNELS * IN_ROWS * IN_COLS;
  int OUT = OUT_CHANNELS * OUT_ROWS * OUT_COLS;

  // size
  Graph g;

  // 2D input, IN_CHANNELS * IN_ROWS, IN_COLS = 4x3;
  //
  //  1  2  3 // channel 1
  //  4  5  6
  //
  //  7  8  9 // channel 2
  // 10 11 12

  // 2D input as vector
  auto& x = *g.new_variable(1, IN_CHANNELS * IN_ROWS * IN_COLS);
  // col major
  //x.value() << 1,4, 2,5, 3,6, 7,10, 8,11, 9,12;
  // row major
  x.value() << 1,2,3, 4,5,6, 7,8,9, 10,11,12;

  // 2D conv layer
  auto& y = *g.new_conv2d(
    x,
    IN_ROWS, IN_COLS,
    IN_CHANNELS, OUT_CHANNELS,
    K_ROWS, K_COLS,
    STRIDE, PADDING, DILATION
  );

  // K, OUT_CHANNELS * K_ROWS, IN_CHANNELS * K_COLS = 6x4
  //
  //  1  2    5  6
  //  3  4    7  8
  //
  //  9 10   11 12
  // 13 14   15 16
  //
  // 17 18   19 20
  // 21 22   23 34

  auto& K = y.K();
  K.value() << 1, 2,  5, 6,
               3, 4,  7, 8,
               9,10, 11,12,
              13,14, 15,16,
              17,18, 19,20,
              21,22, 23,24;

  y.forward();
  g.backward(y, Tensor::Ones(1, OUT));

  // dFdK
  Tensor dFdK = K.gradient();
  ASSERT(dFdK.rows() == OUT_CHANNELS * K_ROWS);
  ASSERT(dFdK.cols() == IN_CHANNELS * K_COLS);

  // dFdK_hat = x
  Tensor dFdK_hat = g.dFdX(y, K);
  ASSERT(dFdK_hat.rows() == OUT_CHANNELS * K_ROWS);
  ASSERT(dFdK_hat.cols() == IN_CHANNELS * K_COLS);
  ASSERT(dFdK.isApprox(dFdK_hat, 0.01))

  // dFdx
  Tensor dFdx = x.gradient();
  ASSERT(dFdx.rows() == 1);
  ASSERT(dFdx.cols() == IN);

  // dFdx_hat = K
  Tensor dFdx_hat = g.dFdX(y, x);
  ASSERT(dFdx_hat.rows() == 1);
  ASSERT(dFdx_hat.cols() == IN);
  ASSERT(dFdx.isApprox(dFdx_hat, 0.01))

  TEST_END()
}

void test_gaussian_sampler()
{
  TEST_BEGIN("Gaussian Sampler")

  long N = 10;
  long M = 100000;
  Graph g;

  Constant m(g, N, 1);
  m.value() = 10 * Tensor::Random(N, 1);

  Constant s(g, N, 1);
  s.value() = Tensor::Random(N, 1);
  s.value()(0) = 0;
  Abs sabs(g, s);

  Sampler n(g, m, sabs);

  // random samples
  auto x = Tensor(N, M);
  for (auto i=0; i<M; i++)
  {
    g.recache();
    n.recache();
    x.block(0,i, N,1) = n.forward();
  }

  // compute mean
  auto mean = x.rowwise().sum() / M;
  ASSERT(mean.isApprox(m(), 0.01))

  // compute std dev
  auto x_m = x.colwise() - mean;
  auto x_m_2 = x_m.array() * x_m.array();
  auto x_m_2_sum = x_m_2.rowwise().sum();
  auto sd = (x_m_2_sum / (M - 1)).sqrt().matrix();
  ASSERT(sd.isApprox(sabs(), 0.1))

  TEST_END()
}

void test_linear_regression()
{
  TEST_BEGIN("Linear Regression")

  // size
  int N = 5;
  srand(time(NULL));
  Graph g;

  // x
  Constant& x = *g.new_constant(1, N);

  // y = W * x + b
  auto& y = *g.new_linear(x, N, N);

  // target y
  Constant& y_hat = *g.new_constant(1, N);

  // target x
  Tensor tx(1, N);

  // target W
  Tensor tW = Tensor::Random(N, N);

  // target b
  Tensor tb = Tensor::Random(1, N);

  // Loss
  auto& diff = *g.new_sub(y_hat, y);
  auto& pow2 = *g.new_mul(diff, diff);
  auto& loss = *g.new_sum(pow2);

  SGD opt(g.variables(), 0.01);

  int steps = 20000;
  int step = 0;
  while(true)
  {
    step += 1;

    // create sample
    tx = Tensor::Random(1, N);
    y_hat.value() = tW * tx + tb;

    // reset input
    g.recache();
    x.value() = tx;

    Tensor l = loss.forward();
    if (l(0) < 1e-3 || step > steps) break;

    // update gradients
    g.backward(loss, l);

    // update weights
    opt.update();

    // clear gradients
    g.zero_grad();
  }

  ASSERT(step < steps)

  TEST_END()
}

void test_quadratic_regression()
{
  TEST_BEGIN("Quadratic Regression")

  // size
  int N = 5;
  srand(time(NULL));
  Graph g;

  // x
  auto& x = *g.new_constant(1, N);

  // x*x
  auto& xx = *g.new_mul(x, x);

  // A
  auto& A = *g.new_variable(N, N);
  ASSERT(A.value().rows() == N)
  ASSERT(A.value().cols() == N)

  // quadratic part
  auto& Axx = *g.new_product(xx, A);

  // Bx_C = B * x + C
  auto& Bx_C = *g.new_linear(x, N, N);

  // y = A * x^2 + B * x + C
  auto& y = *g.new_add(Axx, Bx_C);

  // target y
  auto& y_hat = *g.new_constant(1, N);

  // target x
  Tensor tx(N, 1);

  // target A
  Tensor tA = Tensor::Random(N,N);

  // target B
  Tensor tB = Tensor::Random(N,N);

  // target C
  Tensor tC = Tensor::Random(1,N);

  // Loss
  auto& diff = *g.new_sub(y_hat, y);
  auto& pow2 = *g.new_mul(diff, diff);
  auto& loss = *g.new_sum(pow2);

  SGD opt(g.variables(), 0.01);

  int steps = 20000;
  int step = 0;
  while(true)
  {
    step += 1;

    // create sample
    tx = Tensor::Random(1,N) / 10;
    auto txx = (tx.array() * tx.array()).matrix();
    y_hat.value() = tA * txx + tB * tx + tC;

    // reset input
    g.recache();
    x.value() = tx;

    Tensor l = loss.forward();
    if (l(0) < 1e-3 || step > steps) break;

    // update gradients
    g.backward(loss, l);

    // update weights
    opt.update();

    // clear gradients
    g.zero_grad();
  }

  ASSERT(step < steps)

  TEST_END()
}

void test_average_convergence()
{
  TEST_BEGIN("Average Convergence")

  int N = 5;
  int C = 1000000;

  SMA sma(-2.0, 10);
  EMA ema(-2.0, 0.99);
  WMA wma(-2.0, 10);

  Tensor T = Tensor::Random(N,N) * 12;
  for (int i=0; i<C; i++)
  {
    Tensor x = T + Tensor::Random(N,N);
    sma.update(x);
    ema.update(x);
    wma.update(x, 1.0);
  }

  ASSERT(sma().isApprox(T, 0.01))
  ASSERT(ema().isApprox(T, 0.01))
  ASSERT(wma().isApprox(T, 0.01))

  TEST_END()
}

void test_adam_optimizer()
{
  TEST_BEGIN("Adam Optimizer")
  
  // size
  int N = 5;
  srand(time(NULL));
  Graph g;

  // x
  auto& x = *g.new_constant(1, N);

  // x*x
  auto& xx = *g.new_mul(x, x);

  // A
  auto& A = *g.new_variable(N, N);

  // quadratic part
  auto& Axx = *g.new_product(xx, A);

  // Bx_C = B * x + C
  auto& Bx_C = *g.new_linear(x, N, N);

  // y = A * x^2 + B * x + C
  auto& y = *g.new_add(Axx, Bx_C);

  // target y
  auto& y_hat = *g.new_constant(1, N);

  // target x
  Tensor tx(1, N);

  // target A
  Tensor tA = Tensor::Random(N,N);

  // target B
  Tensor tB = Tensor::Random(N,N);

  // target C
  Tensor tC = Tensor::Random(1,N);

  // Loss
  auto& diff = *g.new_sub(y_hat, y);
  auto& pow2 = *g.new_mul(diff, diff);
  auto& loss = *g.new_sum(pow2);

  Adam opt(g.variables(), 0.01);

  int steps = 20000;
  int batch = 2;
  int step = 0;
  while(batch)
  {
    for (int i=0; i<batch; i++)
    {      
      step += 1;

      // create sample
      tx = Tensor::Random(1,N);
      auto txx = (tx.array() * tx.array()).matrix();
      y_hat.value() = tA * txx + tB * tx + tC;

      // reset input
      g.recache();
      x.value() = tx;

      Tensor l = loss.forward();
      if (l(0) < 1e-3 || step > steps) { batch = 0; break; }

      // update gradients
      g.backward(loss, l);
    }

    // update weights
    opt.update();

    // clear gradients
    g.zero_grad();
  }

  ASSERT(step < steps)

  TEST_END()
}

void test_ImageFP()
{
  TEST_BEGIN("ImageFP")

  Image img(100, 200);

  // image to FP-image

  ImageFP fpi(img.rows(), img.cols(), img.channels());

  auto data = fpi.data();
  auto im_data = img.data();

  ASSERT(fpi.rows() == img.rows());
  ASSERT(fpi.cols() == img.cols());
  ASSERT(fpi.size() == img.size());
  ASSERT(fpi.channels() == img.channels());

  for(auto i=img.size(); i>0; i--) data[i-1] = im_data[i-1];

  ImageFP fp_cropped = fpi.crop(-100,-100, fpi.rows()/2, fpi.cols()/2);
  Image cropped(fp_cropped.rows(), fp_cropped.cols(), fp_cropped.channels());

  data = fp_cropped.data();
  im_data = cropped.data();

  for(auto i=cropped.size(); i>0; i--) im_data[i-1] = data[i-1];
    
  ASSERT(fp_cropped.rows() == cropped.rows());
  ASSERT(fp_cropped.cols() == cropped.cols());
  ASSERT(fp_cropped.size() == cropped.size());
  ASSERT(fp_cropped.channels() == cropped.channels());

  ImageFP fp_scaled = fp_cropped.scale(img.rows(), img.cols());
  Image scaled(fp_scaled.rows(), fp_scaled.cols(), fp_scaled.channels());

  data = fp_scaled.data();
  im_data = scaled.data();

  for(auto i=scaled.size(); i>0; i--) im_data[i-1] = data[i-1];
    
  ASSERT(fp_scaled.rows() == scaled.rows());
  ASSERT(fp_scaled.cols() == scaled.cols());
  ASSERT(fp_scaled.size() == scaled.size());
  ASSERT(fp_scaled.channels() == scaled.channels());

  TEST_END()
}

void test_painter()
{
  TEST_BEGIN("Painter")

  //
  // Square polygon
  //
  // (2,4)  -> (10,4)
  //            \|/
  // (10,4) <- (10,8)
  //

  PointVector square = {
    Point(2,4), Point(10,4), Point(10,8), Point(2,8),
  };

  int rows = 10;
  int cols = 15;

  Painter painter(rows, cols);
  painter.draw_polygon(square);
  auto output = painter.output();

  /*
  Image image(rows, cols, 3);
  memset(image.data(), 0, image.size());
  for (auto it=output.begin(); it!=output.end(); it++)
  {
    auto& pt = *it;
    image.set(pt.y(), pt.x(), 255, 255, 255);
  }
  image.save("/home/greg/Pictures/painter-square.bmp");
  */

  for (int y=0; y<rows; y++)
  for (int x=0; x<cols; x++)
  {
    if (x >= 2 && x <= 10 &&
        y >= 4 && y <= 8)
    {
      ASSERT(output.find(Point(x,y)) != output.end())
    }
    else
    {
      ASSERT(output.find(Point(x,y)) == output.end())
    }
  }

  TEST_END()
}

void test_rl_env()
{
  TEST_BEGIN("RL Env Center")

  int rows = 100;
  int cols = 150;
  
  RLEnv env;
  
  Image image(rows, cols, 3); // random image
  memset(image.data(), 0, rows * cols * 3);
  
  env.set_full_rgb(image.data(), 1, rows, cols);
  
  env.new_episode();
  env.enable_view_frame(true);

  auto full = env.get_full_rgb();
  auto view = env.get_view_rgb();
  
  ASSERT(full.data()[0] == image.data()[0])
  
  // find top-left corner of view in full image
  int frame_col = (full.cols() - view.cols()) / 2;
  int frame_row = (full.rows() - view.rows()) / 2;
  
  // check if view frame is yellow
  ASSERT(full.red(frame_row-1, frame_col-1) == 0x00)
  ASSERT(full.green(frame_row-1, frame_col-1) == 0xFF)
  ASSERT(full.blue(frame_row-1, frame_col-1) == 0xFF)
  
  // check if view matches the full image
  ASSERT(full.red(frame_row, frame_col) == view.red(0,0))
  ASSERT(full.green(frame_row, frame_col) == view.green(0,0))
  ASSERT(full.blue(frame_row, frame_col) == view.blue(0,0))

  TEST_END()

  TEST_BEGIN("RL Env Corner")

  int rows = 100;
  int cols = 150;
  int view_rows = 20;
  int view_cols = 20;
    
  RLEnv env;
  env.set_view_size(view_rows, view_cols);
  
  Image image(rows, cols, 3); // random image
  memset(image.data(), 0, rows * cols * 3);
  
  env.set_full_rgb(image.data(), 1, rows, cols);
  
  env.new_episode();
  env.enable_full_frame(true);
  env.enable_view_frame(true);

  // move view to bottom-right corner of the full image
  env.action_horizontal(0.5*cols/view_cols);
  env.action_vertical(0.5*rows/view_rows);

  auto full = env.get_full_rgb();
  auto view = env.get_view_rgb();
  
  full.save("/home/greg/Pictures/rl-corner-full.bmp");
  view.save("/home/greg/Pictures/rl-corner-view.bmp");

  ASSERT(full.data()[0] == image.data()[0])
  
  // find bottom-right corner of the full image in the view
  int frame_col = view_cols / 2;
  int frame_row = view_rows / 2;
  
  // check if full frame is yellow
  ASSERT(view.red(frame_row, frame_col) == 0x00)
  ASSERT(view.green(frame_row, frame_col) == 0xFF)
  ASSERT(view.blue(frame_row, frame_col) == 0xFF)
  
  // check if view matches the full image
  ASSERT(full.red(rows-1, cols-1) == view.red(frame_row-1, frame_col-1))
  ASSERT(full.green(rows-1, cols-1) == view.green(frame_row-1, frame_col-1))
  ASSERT(full.blue(rows-1, cols-1) == view.blue(frame_row-1, frame_col-1))

  TEST_END()
}

/**
 * test entry point
 */ 
int main(int argc, char* argv[]) {

  test_ImageFP();
  test_painter();
  test_rl_env();

  test_eigen_fft();
  test_audio_file();
  test_image_file();

  test_eigen_matrix();
  test_random_numbers();
  test_discount_reward();
  test_cosine_similarity();
  test_function_negative();
  test_function_names();

  test_constant();
  test_variable();

  test_numerical_derivative();
  test_back_propagation();

  test_broadcast_forward();
  test_broadcast_backward();

  test_split_forward();
  test_split_backward();

  test_join_forward();
  test_join_backward();

  test_min_forward();
  test_min_backward();

  test_max_forward();
  test_max_backward();

  test_reshape_forward();
  test_reshape_backward();

  test_linear_forward();
  test_linear_backward();

  test_product_forward();
  test_product_backward();

  test_add_forward();
  test_add_backward();

  test_sub_forward();
  test_sub_backward();

  test_mul_forward();
  test_mul_backward();

  test_power_forward();
  test_power_backward();

  test_tanh_forward();
  test_tanh_backward();

  test_sigmoid_forward();
  test_sigmoid_backward();

  test_relu_forward();
  test_relu_backward();

  test_erf_forward();
  test_erf_backward();

  test_gelu_forward();
  test_gelu_backward();

  test_dropout_forward();
  test_dropout_backward();

  test_softmax_forward();
  test_softmax_backward();

  test_softplus_forward();
  test_softplus_backward();

  test_log_softmax_forward();
  test_log_softmax_backward();

  test_log_forward();
  test_log_backward();

  test_sum_forward();
  test_sum_backward();

  test_abs_forward();
  test_abs_backward();

  test_transpose_forward();
  test_transpose_backward();

  test_mean_forward();
  test_mean_backward();

  test_stack_forward();
  test_stack_backward();

  test_gru_forward();
  test_gru_backward();

  test_norm_forward();
  test_norm_backward();

  test_gaussian_forward();
  test_gaussian_backward();

  test_log_gaussian_forward();
  test_log_gaussian_backward();

  test_embedding_forward();
  test_embedding_backward();

  test_conv2d_forward();
  test_conv2d_backward();

  test_gaussian_sampler();
  test_linear_regression();
  test_quadratic_regression();

  test_average_convergence();
  test_adam_optimizer();

  return 0;
}

