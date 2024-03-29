# Seegnify

[Seegnify](https://www.seegnify.org/) is a machine learning library that runs anywhere.

## Main Features

  * Written in C++ for performance, portability and usability
  * Automatic differentiation for training
  * SGD, Adam/AdamNC, Yogi and RMSprop optimizers
  * Extendable generic tensor with default Eigen backend
  * No third-party dependencies in main module
  * Zero memory allocations during runtime
  * Out-of-the-box distributed training option

## More About Seegnify

<!-- toc -->

- [Project Structure](#project-structure)
- [Installation](#installation)
  - [From Source](#from-source)
  - [Unit Test](#unit-test)
  - [Full Test](#full-test)
- [Examples](#examples)
- [Training](#training)
- [Android Support](#android-support)

<!-- tocstop -->

### Project Structure

| Component | Description                                        |
| --------- | -------------------------------------------------- |
| bin       | Build and run-time control scripts for convenience |
| main      | Deep-Learning graph with automatic differentiation |
| utils     | Utilities supporting distributed learning          |
| external  | External source code for data IO and run-time      |
| examples  | Neural Network models and training examples        |

## Installation

### From Source

Compile the binaries using provided CMake wrapper script:

```bash
./bin/build.sh
```

### Unit test

Executed all unit tests:

```bash
./build/seegnify-unittest
```

### Full test

Start a full end-to-end distributed training test:

```bash
./bin/start.sh
```

Monitor training progress:

```bash
tail -f worker.log
```

Stop the worker and the master processes:

```bash
./bin/stop.sh
```

## Examples

The following training code examples can be found in the `examples` folder:

| Example    | Description                                                  |
| ---------- | ------------------------------------------------------------ |
| MNIST      | Model definition, training and validation on MNIST dataset   |
| CIFAR10    | Model definition, training and validation on CIFAR10 dataset |
| MINST RL   | Reinforcement learning with attention on MNIST dataset       |
| CIFAR10 RL | Reinforcement learning with attention on CIFAR10 dataset     |

## Training

A complete reference implementation for single or multi-process training can
be found in the `examples` folder as well as in the full test suite. To train
in a single process, run the training loop within a single executable. To
train in a multi-process mode, compile the model as a shared object `(.so)`
and start the master and worker processes. To train in distributed mode, run
the workers on multiple machines and point them to the server machine hosting
the master process.

In the example below, the master process listens to workers on port 2020. The
workers connect to `localhost` on port `2020`. By default, a single worker
utilizes all available cores and assigns one training instance to each core.

Start the master process on port 2020 and store the model in my.graph:

```bash
./build/seegnify-training master my.graph 2020
```

Start the worker process with your compiled model and connect to master:

```bash
./build/seegnify-training worker 127.0.0.1 2020 ./build/libseegnify-fulltest.so
```

Stop the training by sending SIGINT (Ctrl-C) signal to the master process.

