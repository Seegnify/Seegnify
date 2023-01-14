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

#ifndef _SEEGNIFY_RL_ENV_H_
#define _SEEGNIFY_RL_ENV_H_

#include <cstdint>
#include <string>
#include <iostream>
#include "image.hh"

namespace seegnify {

// RL environment
class RLEnv
{
public:
    RLEnv();
    virtual ~RLEnv();

    ////////////////////////// instance RL API /////////////////////////////////

    uint16_t get_actions_count();

    bool is_episode_finished();

    float make_action(uint16_t action);

    void new_episode();

    void set_data_rgb(uint8_t* rgb, uint16_t depth, uint16_t rows, uint16_t cols);
    void set_view_size(uint16_t rows, uint16_t cols);

    ////////////////////////// instance UI API /////////////////////////////////

    void get_data_size(uint16_t& rows, uint16_t& cols);
    void get_view_size(uint16_t& rows, uint16_t& cols);

    Image get_data_rgb();
    Image get_view_rgb();

    std::string get_info();
    void enable_view_frame(bool show) { show_view_frame = show; }

    ////////////////////////// instance actions ////////////////////////////////

    enum ACTION
    {
      ACTION_UP,
      ACTION_DOWN,
      ACTION_LEFT,
      ACTION_RIGHT,
      ACTION_FORWARD,
      ACTION_BACKWARD,
      ACTION_ZOOM_IN,
      ACTION_ZOOM_OUT,
      ACTION_END,
    };

    void action_up();
    void action_down();
    void action_left();
    void action_right();
    void action_forward();
    void action_backward();
    void action_zoom_in();
    void action_zoom_out();

    void action_horizontal(float rx);
    void action_vertical(float ry);
    void action_deep(float rz);
    void action_zoom(float zoom);

    ////////////////////////// private methods /////////////////////////////////

protected:
    uint32_t data_row(uint32_t view_row);
    uint32_t data_col(uint32_t view_col);
    uint32_t view_row(uint32_t data_row);
    uint32_t view_col(uint32_t data_col);

    virtual void reset();
    virtual void clear();

    void draw_agent_frame(Image& img);

    virtual float get_reward();

protected:

    // data
    bool finished;
    uint8_t *data;
    uint16_t slices;
    uint16_t data_rows;
    uint16_t data_cols;
    uint16_t view_rows;
    uint16_t view_cols;

    // position
    float slice;
    float x, y;
    float scale;

    // action cache
    float total_reward;
    uint16_t last_action;
    uint32_t action_step;

    // UI
    bool show_view_frame;
};

} /* namespace */

#endif
