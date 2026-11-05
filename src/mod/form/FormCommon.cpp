#include "mod/form/FormCommon.h"

#include <algorithm>
#include <charconv>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/form/SimpleForm.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mod/Mod.h"
#include "mod/core/Util.h"
#include "mod/i18n/Text.h"

namespace ib {

namespace {
// 每页条数由配置项 pageSize 控制（1-50），这里只给个兜底
size_t pageSize() {
    auto const value = currentConfig().pageSize;
    return static_cast<size_t>(value < 1 ? 1 : (value > 50 ? 50 : value));
}
using ItemsPtr = std::shared_ptr<std::vector<MenuItem>>;
} // namespace

void showPage(Player& player, std::string const& title, ItemsPtr items, MenuAction back, size_t page);

namespace {

void appendButton(ll::form::SimpleForm& form, MenuItem const& item) {
    if (item.icon.empty()) {
        form.appendButton(item.label, [item](Player& who) { item.action(who); });
    } else {
        form.appendButton(item.label, item.icon, "path", [item](Player& who) { item.action(who); });
    }
}

// 老插件那种「跳转到...」：输入页码直接翻，输错退回当前页
void addJumpButton(
    ll::form::SimpleForm& form,
    LocaleText const&     T,
    std::string const&    title,
    ItemsPtr              items,
    MenuAction            back,
    size_t                page,
    size_t                pages
) {
    form.appendButton(
        T("backup.common.jump"),
        [title, items, back, page, pages](Player& who) {
            auto const            T2 = textFor(who);
            ll::form::CustomForm jump{T2("backup.common.jump_title")};
            jump.appendInput("page", T2("backup.common.jump_input", pages), "", std::to_string(page + 1));
            jump.sendTo(
                who,
                [title, items, back, page, pages](
                    Player&                     target,
                    ll::form::CustomFormResult const& result,
                    ll::form::FormCancelReason
                ) {
                    auto const T    = textFor(target);
                    auto const stay = [&]() { showPage(target, title, items, back, page); };
                    if (!result) return stay();

                    auto it = result->find("page");
                    if (it == result->end()) return stay();
                    auto const* text = std::get_if<std::string>(&it->second);
                    if (text == nullptr || text->empty()) return stay();

                    size_t      targetPage = 0;
                    auto const  begin      = text->data();
                    auto const  end        = begin + text->size();
                    auto [ptr, ec]         = std::from_chars(begin, end, targetPage);
                    if (ec != std::errc{} || ptr != end || targetPage == 0 || targetPage > pages) {
                        tellError(target, T("backup.common.jump_invalid", pages));
                        return stay();
                    }
                    showPage(target, title, items, back, targetPage - 1);
                }
            );
        }
    );
}

} // namespace

void showPage(
    Player&            player,
    std::string const& title,
    ItemsPtr           items,
    MenuAction         back,
    size_t             page
) {
    auto const T = textFor(player);

    size_t const perPage = pageSize();
    size_t const total   = items->size();
    size_t       pages   = (total + perPage - 1) / perPage;
    if (pages == 0) pages = 1;
    if (page >= pages) page = pages - 1;

    std::string content = T("backup.common.page", page + 1, pages) + "\n" + T("backup.common.item_count", total);

    ll::form::SimpleForm form{title, content};

    // 老插件的翻页布局：上一页在最上面、下一页在最下面，首尾循环
    if (pages > 1) {
        form.appendButton(T("backup.common.prev"), [title, items, back, page, pages](Player& who) {
            showPage(who, title, items, back, page == 0 ? pages - 1 : page - 1);
        });
        addJumpButton(form, T, title, items, back, page, pages);
    }

    size_t const begin = page * perPage;
    size_t const end   = std::min(total, begin + perPage);
    for (size_t i = begin; i < end; ++i) appendButton(form, (*items)[i]);

    if (pages > 1) {
        addJumpButton(form, T, title, items, back, page, pages);
        form.appendButton(T("backup.common.next"), [title, items, back, page, pages](Player& who) {
            showPage(who, title, items, back, page + 1 >= pages ? 0 : page + 1);
        });
    }
    if (back) form.appendButton(T("backup.common.back"), [back](Player& who) { back(who); });
    form.sendTo(player);
}

void tell(Player& player, std::string const& text) {
    auto packet = TextPacket::createRawMessage(msgPrefix() + text);
    player.sendNetworkPacket(packet);
}

void tellOk(Player& player, std::string const& text) { tell(player, "§a" + text); }

void tellError(Player& player, std::string const& text) { tell(player, "§c" + text); }

void showNotice(Player& player, std::string const& title, std::string const& content, MenuAction back) {
    auto const           T = textFor(player);
    ll::form::SimpleForm form{title, content};
    if (back) form.appendButton(T("backup.common.back"), [back](Player& who) { back(who); });
    form.sendTo(player);
}

void confirm(
    Player&            player,
    std::string const& title,
    std::string const& content,
    std::string const& upperButton,
    MenuAction         onConfirm,
    MenuAction         onCancel
) {
    auto const            T = textFor(player);
    ll::form::ModalForm form{title, content, upperButton, T("backup.common.cancel")};
    form.sendTo(player, [onConfirm, onCancel](Player& who, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
        if (result && *result == ll::form::ModalFormSelectedButton::Upper) {
            onConfirm(who);
        } else if (onCancel) {
            onCancel(who);
        }
    });
}

} // namespace ib
