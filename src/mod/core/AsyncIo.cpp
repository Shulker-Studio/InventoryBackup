#include "mod/core/AsyncIo.h"

#include "ll/api/thread/ServerThreadExecutor.h"
#include "ll/api/thread/ThreadPoolExecutor.h"
#include "mod/Mod.h"

namespace ib {

void runIo(std::function<void()> task) {
    if (currentConfig().backgroundIo) {
        ll::thread::ThreadPoolExecutor::getDefault().execute(std::move(task));
    } else {
        task();
    }
}

void backToServer(std::function<void()> task) {
    ll::thread::ServerThreadExecutor::getDefault().execute(std::move(task));
}

} // namespace ib
