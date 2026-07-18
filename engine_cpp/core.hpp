#pragma once
/*
 * core.hpp — main engine entry point
 */
#include <string>

// Run the game. Blocks until window is closed.
void run_game(const std::string& scene_path,
              int   width  = 1280,
              int   height = 720,
              bool  vsync  = true);
