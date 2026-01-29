#pragma once

#include <concepts>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "Logger.h"

namespace ModuleManager {

// 在线程入口自动设置模块名上下文，避免日志落到 moduleManager。
template <class F, class... Args>
requires std::invocable<F&, std::stop_token, Args...>
std::jthread StartModuleThread(std::string moduleName, F&& fn, Args&&... args) {
  return std::jthread(
      [name = std::move(moduleName),
       func = std::forward<F>(fn)](std::stop_token st, Args... innerArgs) mutable {
        LogModuleScope scope(name);
        std::invoke(func, st, std::move(innerArgs)...);
      },
      std::forward<Args>(args)...);
}

template <class F, class... Args>
requires (!std::invocable<F&, std::stop_token, Args...> && std::invocable<F&, Args...>)
std::jthread StartModuleThread(std::string moduleName, F&& fn, Args&&... args) {
  return std::jthread(
      [name = std::move(moduleName),
       func = std::forward<F>(fn)](Args... innerArgs) mutable {
        LogModuleScope scope(name);
        std::invoke(func, std::move(innerArgs)...);
      },
      std::forward<Args>(args)...);
}

}  // namespace ModuleManager
