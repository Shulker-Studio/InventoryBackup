#include "mod/i18n/Text.h"

#include "ll/api/io/Logger.h"
#include "mod/Mod.h"

namespace ib {

void loadTexts() {
    auto const dir = Mod::getInstance().getSelf().getLangDir();
    if (auto loaded = ll::i18n::getInstance().load(dir); !loaded) {
        getLogger().warn("载入 lang/ 失败（{}）：{}；界面文案会显示成 key", dir.string(), loaded.error().message());
    }
}

} // namespace ib
