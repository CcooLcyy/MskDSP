#pragma once

#include <concepts>
#include <exception>
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
        const std::string moduleNameCopy = name;
        LogModuleScope scope(moduleNameCopy);
        try {
          std::invoke(func, st, std::move(innerArgs)...);
        } catch (const std::exception& ex) {
          LOG_ERROR("模块线程异常: 模块={}, 原因={}", moduleNameCopy, ex.what());
        } catch (...) {
          LOG_ERROR("模块线程异常: 模块={}, 原因=未知异常", moduleNameCopy);
        }
      },
      std::forward<Args>(args)...);
}

template <class F, class... Args>
requires (!std::invocable<F&, std::stop_token, Args...> && std::invocable<F&, Args...>)
std::jthread StartModuleThread(std::string moduleName, F&& fn, Args&&... args) {
  return std::jthread(
      [name = std::move(moduleName),
       func = std::forward<F>(fn)](Args... innerArgs) mutable {
        const std::string moduleNameCopy = name;
        LogModuleScope scope(moduleNameCopy);
        try {
          std::invoke(func, std::move(innerArgs)...);
        } catch (const std::exception& ex) {
          LOG_ERROR("模块线程异常: 模块={}, 原因={}", moduleNameCopy, ex.what());
        } catch (...) {
          LOG_ERROR("模块线程异常: 模块={}, 原因=未知异常", moduleNameCopy);
        }
      },
      std::forward<Args>(args)...);
}

}  // namespace ModuleManager
