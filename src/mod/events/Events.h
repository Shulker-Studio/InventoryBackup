#pragma once

namespace ib {

void registerEvents();

void unregisterEvents();

// 在线定时备份 + 过期清理（后台线程，到点切回服务端线程执行）
void startScheduler();

void stopScheduler();

} // namespace ib
