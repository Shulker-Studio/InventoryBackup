#pragma once

#include <functional>

namespace ib {

// 纯 CPU / 纯 I/O 的活（压缩、读写盘）丢给线程池，不占服务端线程。
// config.backgroundIo = false 时 task 直接在调用线程上跑完（此时就等于全同步了，
// 用来排查是不是并发引起的问题）；backToServer 那一步不受影响，仍然要回服务端线程。
// 任务里不许碰 Player、世界库、BackupStore 的索引/任务状态，也不许调 msg() 和表单：
// 那些都只在服务端线程上安全。
void runIo(std::function<void()> task);

// 后台任务的收尾必须回服务端线程执行：提交索引、通知玩家、写日志都只在这边做
void backToServer(std::function<void()> task);

} // namespace ib
