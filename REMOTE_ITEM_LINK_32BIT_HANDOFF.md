# 远程查看玩家装备 Item Link 32 位属性修复交接文档

## 1. 问题背景

远程查看其他玩家装备时，`GetItem()` 或装备栏生成的 Item Link 中，Random Property 只剩 16 位。

示例：

```text
完整值：232046
十六进制：0x00038A6E
截断值：35438
十六进制：0x00008A6E
```

邮件、拍卖行、自身装备的 Item Link 正常，只有远程玩家装备的原生路径会被截断。

根本原因是远程玩家装备使用 Visible Item 数据，客户端原生路径将 `PLAYER_VISIBLE_ITEM_*_PROPERTIES` 按低 16 位读取。

## 2. 服务端要求

服务端必须把完整 32 位值写入 Visible Item 的第一个 Properties DWORD：

```cpp
PLAYER_VISIBLE_ITEM_1_PROPERTIES +
    slot * MAX_VISIBLE_ITEM_OFFSET
```

对应客户端偏移：

```cpp
OFF_VISIBLE_ITEM_PROPERTIES = 0x28
```

当前客户端方案读取的是第一个 DWORD：

```text
entry + 0x28
```

不要依赖 `entry + 0x2C`。`dwords[11]` 是后续字段，不能作为正式属性来源。

用户实际验证过：

```text
properties = 0x00038A6E
```

说明完整 32 位值已经成功到达客户端。

相关服务端工程：

```text
E:\MaNGOS_Turtle\patch_1171
```

## 3. 客户端工程和提交

客户端工程目录：

```text
E:\魔兽逆向资料\112\wowhookStudy\ClassicAPI
```

原始提交：

```text
37a924d69881fd0a2af7f70fa26475b8bb8bedd0
```

清理提交：

```text
41feac8 清理垃圾
```

交接文档提交：

```text
cb38f74 开发交接文档
```

## 4. 当前客户端修复逻辑

### 4.1 关键偏移

文件：`src/Offsets.h`

```cpp
FUN_SCRIPT_GET_INVENTORY_ITEM_LINK = 0x004C8C10;
FUN_UNIT_GET_VISIBLE_ITEM = 0x005F0D60;
OFF_VISIBLE_ITEM_ITEM_ID = 0x08;
OFF_VISIBLE_ITEM_PROPERTIES = 0x28;
```

### 4.2 Item Link 构造

文件：`src/item/Link.cpp`、`src/item/Link.h`

新增 `BasicFromIDProperty(uint32_t itemID, uint32_t property, ...)`，直接把完整 32 位属性写入 Item Link：

```text
|Hitem:itemID:0:property:0:0:0:0:0|
```

这里不把属性当作普通 DBC 的 Random Property ID 查询，因为服务端传入的值可能实际是物品 `GUIDLow`，不一定存在对应 DBC 行。

### 4.3 远程装备读取

文件：`src/item/TooltipItem.cpp`

处理流程：

1. 解析 `target` 等单位 Token。
2. 获取远程玩家对象。
3. 调用 `FUN_UNIT_GET_VISIBLE_ITEM`。
4. 读取装备 ID：`entry + 0x08`。
5. 读取完整属性：`entry + 0x28`。
6. 生成完整 Item Link。

### 4.4 GetInventoryItemLink

`GetInventoryItemLink_h()` 会优先处理远程玩家装备：

```lua
GetInventoryItemLink("target", slot)
```

远程玩家使用自定义的完整 32 位链接；本地玩家和普通物品继续使用原生路径。

### 4.5 GameTooltip:GetItem()

文件：`src/item/Tooltip.cpp`

当 Tooltip 对应远程玩家装备时，从 `CurrentVisibleItem()` 获取完整属性，并使用 `BasicFromIDProperty()` 返回完整 Item Link。

### 4.6 pending 状态

`SetInventoryItem()` 和 `SetHyperlink()` 的原生流程会在函数内部触发 Tooltip 回调，因此维护两个状态：

```cpp
g_visibleItem;
g_pendingVisibleItem;
```

`g_pendingVisibleItem` 保证原生构建 Item Link 期间，`GameTooltip:GetItem()` 仍然能够读取完整 32 位属性。

`ClearVisibleItem()` 只清理已经发布的状态，故意保留 pending 状态直到原生回调完成。

### 4.7 `/reload` 状态清理

`src/item/TooltipItem.cpp` 中注册了 `PrepareForReload()`：

```cpp
void PrepareForReload() {
    g_visibleItem = {};
    g_pendingVisibleItem = {};
}
```

作用是避免 `/reload` 后旧 Tooltip 指针或旧装备状态残留。

## 5. 已移除的不相关代码

已从工作区删除：

```text
src/item/VisibleItemDebug.cpp
```

该文件只是临时调试 API：

```lua
C_Item.GetVisibleItemRaw()
```

它不属于正式修复链路。

同时移除：

```cpp
OFF_VISIBLE_ITEM_PAD = 0x2C;
```

该偏移只被调试文件使用。正式代码不依赖 `GetVisibleItemRaw()` 或 `dwords[11]`。

## 6. 当前工作区状态

代码清理已经提交到 `41feac8`。当前工作区包含交接文档提交 `cb38f74`，状态正常。

清理提交涉及：

```text
M  src/Offsets.h
M  src/item/TooltipItem.cpp
M  src/item/TooltipItem.h
D  src/item/VisibleItemDebug.cpp
```

以上修改已经包含在 `41feac8` 中，当前没有待提交的源码清理修改。

## 7. 验证结果

已执行：

```text
git diff --check
```

检查通过。

已执行 Release 编译：

```text
cmake --build build --config Release -- /m:2
```

结果：

```text
EXIT_CODE=0
```

生成 DLL：

```text
build\Release\ClassicAPI.dll
```

编译中的 `C4819` 是项目原有的中文编码警告，没有新增编译错误。

用户已经确认远程装备的 Item Link 可以正常获得完整 32 位属性。

## 8. 后续排查建议

如果后续出现回归，优先检查：

1. 服务端是否写入第一个 Properties DWORD。
2. 客户端 `entry + 0x28` 是否仍能读到完整值。
3. 是否误将 `entry + 0x2C` 当成属性来源。
4. `/reload` 后是否重新加载了最新 DLL。
5. 观察装备栏是否使用了最新编译的 `ClassicAPI.dll`。

## 9. 已知注意事项

当前实现使用全局的 `g_visibleItem` 和 `g_pendingVisibleItem` 状态，主要服务于当前 Tooltip 构建流程。暂未扩展为多个 Tooltip 并发场景的状态表，因为这不属于本次 32 位属性修复的必要范围。

## 10. 2026-08-27 最终实现（以本节为准）

> 本节覆盖前文中“不要使用 `entry + 0x2C`”以及邮箱字段偏移的旧结论。前文保留为排错历史。

### 10.1 总体方案

不要把 `PLAYER_VISIBLE_ITEM_*_PROPERTIES` 的第一个 DWORD 强行改为
32 位 GUIDLow。客户端会将该字段按两个 16 位值解释，修改该字段会导致
部分武器模型不显示。

保持第一个 DWORD 的原有 Random Property 用途，将 GUIDLow 放入第二个
Properties DWORD（suffix factor）并写入 Item Link 的第四字段：

```text
|Hitem:<itemID>:<enchantID>:<randomProperty>:<uniqueID>|h[Name]|h|r
```

其中 `uniqueID` 是本服的物品 `GUIDLow`。

### 10.2 服务端约定

服务端工程：`E:\MaNGOS_Turtle\patch_1171`

```cpp
uint32 GetItemSuffixFactor() const { return GetGUIDLow(); }
```

物品创建后还应将 `ITEM_FIELD_PROPERTY_SEED` 初始化为同一个 GUIDLow，确保
物品在邮件、拍卖行和可见装备等包路径中都携带一致的 suffix factor。

玩家可见装备保持原有两条写入：

```cpp
SetInt16Value(PLAYER_VISIBLE_ITEM_1_PROPERTIES + slot * MAX_VISIBLE_ITEM_OFFSET,
              0, pItem->GetItemRandomPropertyId());
SetUInt32Value(PLAYER_VISIBLE_ITEM_1_PROPERTIES + 1 + slot * MAX_VISIBLE_ITEM_OFFSET,
               pItem->GetItemSuffixFactor());
```

不要把第一条 `SetInt16Value` 改成 32 位写入。

### 10.3 ClassicAPI 字段映射

客户端工程：`E:\魔兽逆向资料\112\wowhookStudy\ClassicAPI`

远程可见装备：

```text
entry + 0x28  randomProperty
entry + 0x2C  uniqueID / GUIDLow
```

拍卖行条目：

```text
entry + 0x08  itemID
entry + 0x0C  enchantID
entry + 0x10  randomProperty
entry + 0x14  uniqueID / GUIDLow
```

邮件条目必须按服务端 `SMSG_MAIL_LIST_RESULT` 的字段顺序读取：

```text
entry + 0x120 itemID
entry + 0x124 enchantID
entry + 0x128 randomProperty
entry + 0x12C suffixFactor = uniqueID / GUIDLow
```

`+0x134` 及之后是 count、charges、durability 等邮件显示数据，不能作为
uniqueID。曾经把 `+0x134` 当作 uniqueID、把 `+0x138` 当作
randomProperty，均为错误映射。

### 10.4 客户端实现入口

`Item::Link::BasicFromIDPropertyUnique()` 和
`BasicFromIDEnchantPropertyUnique()` 负责生成四字段链接。

以下入口都应使用相同的字段约定：

1. `GetInventoryItemLink("target", slot)`：远程装备读取 `+0x28/+0x2C`。
2. `GameTooltip:SetInventoryItem`：在原生 Tooltip 回调期间通过 pending
   状态保留远程装备的完整字段。
3. `GetAuctionItemLink(type, index)` 以及 `GameTooltip:SetAuctionItem`：
   读取拍卖行条目的四个字段。
4. `GetInboxItemLink(messageIndex[, attachmentIndex])` 以及
   `GameTooltip:SetInboxItem`：读取邮件条目的 `+0x120/+0x124/+0x128/+0x12C`。

原生 Tooltip 在 `Set*Item` 尚未返回时就会触发 `OnTooltipSetItem`；因此
`g_pendingVisibleItem` 在原生构建期间必须保留，不能由 Tooltip 的 Clear
路径提前清掉。

### 10.5 TurtleEnchant 插件约定

插件文件：

```text
E:\twmoa_1181\Interface\AddOns\TurtleEnchant\TurtleEnchant.lua
```

解析四字段链接：

```lua
|Hitem:(%d+):(%d+):(%d+):(%d+)
```

第四字段优先作为 GUIDLow；为兼容旧链接，第四字段为 `0` 时才回退到第三字段。
插件不需要包装 `GameTooltip:SetInboxItem` 或缓存 `teInboxMailID`；该尝试已
撤回，正式入口由 ClassicAPI 的 itemlink 生成逻辑处理。

### 10.6 已排除的错误路径

1. 不使用 DbgView / `OutputDebugString` 诊断。
2. 不在正式版本中保留 `ClassicAPI_InboxItemLink.log` 文件写入。
3. 不修改或禁用 `D:\登录器\ares-login\DllReadFile` 的既有邮箱 Hook；它
   影响旧客户端的 randomProperty 显示，不是 ClassicAPI 的正式修复入口。
4. 不把 `GameTooltip:SetInboxItem` 注册为早期 Lua 表覆盖；初始化阶段可能
   创建或覆盖 `GameTooltip` 全局并造成客户端启动报错。

### 10.7 验证清单

1. 远程观察装备、拍卖行物品、邮箱附件的链接均包含四个数值字段。
2. 第三字段仍为正确的 randomProperty。
3. 第四字段等于对应物品的 GUIDLow。
4. 完全退出客户端后重新注入最新 `build\Release\ClassicAPI.dll`。
5. `TurtleEnchant` 的 `ParseItemLink` 能优先取得第四字段的 GUIDLow。
