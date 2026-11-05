#include "mod/i18n/Text.h"

#include "ll/api/io/Logger.h"
#include "mod/Mod.h"
#include "mod/core/Util.h"

namespace ib {

void loadTexts() {
    auto const dir = Mod::getInstance().getSelf().getLangDir();
    if (auto loaded = ll::i18n::getInstance().load(dir); !loaded) {
        getLogger().warn(msg("log.i18n.load_failed", formatPath(dir), loaded.error().message()));
    }
}

} // namespace ib
