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

清理后的未提交修改：

```text
M  src/Offsets.h
M  src/item/TooltipItem.cpp
M  src/item/TooltipItem.h
D  src/item/VisibleItemDebug.cpp
```

当前尚未创建新的 Git 提交。

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

## 8. 后续提交建议

确认无误后，可提交本次清理：

```bash
git add src/Offsets.h src/item/TooltipItem.cpp src/item/TooltipItem.h src/item/VisibleItemDebug.cpp
git commit -m "清理远程装备32位属性修复代码"
```

如果后续出现回归，优先检查：

1. 服务端是否写入第一个 Properties DWORD。
2. 客户端 `entry + 0x28` 是否仍能读到完整值。
3. 是否误将 `entry + 0x2C` 当成属性来源。
4. `/reload` 后是否重新加载了最新 DLL。
5. 观察装备栏是否使用了最新编译的 `ClassicAPI.dll`。

## 9. 已知注意事项

当前实现使用全局的 `g_visibleItem` 和 `g_pendingVisibleItem` 状态，主要服务于当前 Tooltip 构建流程。暂未扩展为多个 Tooltip 并发场景的状态表，因为这不属于本次 32 位属性修复的必要范围。

