#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Player;

namespace ib {

using MenuAction = std::function<void(Player&)>;

struct MenuItem {
    std::string label;
    MenuAction  action;
    std::string icon; // textures/ui/xxx.png；留空则无图标
};

// 带模组前缀发一条原始文本（Raw 包才不会被吃掉 § 颜色码）
void tell(Player& player, std::string const& text);

void tellOk(Player& player, std::string const& text);

void tellError(Player& player, std::string const& text);

// 分页按钮列表：items 里的 action 已经绑定好归属，翻页只是重新渲染
void showPage(
    Player&                                player,
    std::string const&                     title,
    std::shared_ptr<std::vector<MenuItem>> items,
    MenuAction                             back,
    size_t                                 page = 0
);

void showNotice(Player& player, std::string const& title, std::string const& content, MenuAction back);

void confirm(
    Player&            player,
    std::string const& title,
    std::string const& content,
    std::string const& upperButton,
    MenuAction         onConfirm,
    MenuAction         onCancel
);

} // namespace ib
