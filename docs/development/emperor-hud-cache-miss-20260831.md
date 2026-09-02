# 皇帝仍显示原皮：高位 HUD 手臂未命中缓存

日期：2026-08-31。继续处理本地实际 Demo 复测失败；不把目录兼容性通过等同于画面成功。

## 本次实机证据

已检查当前 CS2（PID 236432）的模块和启动参数：HLAE 2.191.1、`-insecure`，加载的是 `build/paint-compat-fix/zh-CN/nSkinzCS2.zh-CN.dll`，SHA-256 为 `996700334476C943905277AC80ED183D471EC11264F04AF598DDB3F03C73C93B`。

09:24:33 的本地日志记录：

- 玩家 XUID 76561199549726257，M4A4 definition=16，paint=844，原皮意图为 false。
- 属性原地同步成功，世界武器 entity=1898 写入 mesh=2，native material refresh 执行一次。
- `hud_entity=-1`、`hud_render_only=0`、`world fields ONLY; HUD unresolved`。

使用只读 `OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)` 和 `ReadProcessMemory` 获取了该武器→所属玩家→手臂→枪身的有界快照。普通权限首次返回 access denied，经过提升权限许可后只读成功。没有写入、挂起、注入或操控当前 CS2。

快照 `build/temp/emperor-hud-live-20260831.json` 显示：

- 世界武器完整句柄 `0x01EC076A`；玩家句柄 `0x002D0192`；玩家 active handle 与世界武器一致。
- 玩家 `m_hHudModelArms=0xE966429A`，实体槽位 **17050**，实际对象是 `C_CS2HudModelArms`，owner、identity 回指和场景回指正确。
- 手臂子树内有烟雾弹、M9、M4A4 三个 `C_CS2HudModelWeapon`。只有 M4A4 owner 完整句柄等于 `0x01EC076A`，模型为 `weapons/models/m4a4/weapon_rif_m4a4.vmdl`，当时 mesh=1。
- 因而实际存在可验证的第一人称 M4A4；不是只能找到世界武器，也不应把休眠的刀/投掷物 HUD 当作歧义。

只读工具的布局来自本机 Client 2000899 的 Schema 描述和 `GetEntityFromIndex` 反汇编，不调用任何游戏函数。此快照不是截图，也没有证明修复后的视觉效果。

## 修复

1. 原函数只使用 `entity_cache[arms_index]`。普通实体扫描可在连续空槽后提前结束，客户端 HUD 的高位槽位不保证进入该缓存。
2. 当缓存缺失或完整句柄不匹配时，通过已绑定的游戏 `GetEntityFromIndex` 查询**这一个明确的手臂槽位**；随后重新验证完整句柄、owner、RTTI 和场景回指。没有扩大缓存或全实体扫描，也没有按槽位忽略 serial。
3. 继续在最多 64 个场景节点中寻找 owner 等于当前武器完整句柄的唯一 HUD。其它已收起的武器 HUD 被跳过；两个同属当前武器的候选仍拒绝。
4. 若当前观察的活动枪械需要改饰面，玩家也给出了有效 HUD arms 句柄，但仍未找到枪身，则在绑定后、属性/网格/挂件/预缓存/刷新之前返回 pending。不会把该情况报告为完整应用成功。没有 HUD arms 的世界视角不强制要求生成第一人称对象。
5. 不修改 render-only HUD 的 econ 属性载体身份，不把它变成刀模链；属性仍在世界 econ 载体上，网格同步到经过验证的 HUD。

## 验证

- 8 项涂装定向测试入口通过，含真实 34 枪型/1,449 组合、M4A4 844/1353 网格切换。
- 生产 HUD walker 的夹具新增真实高位 arms 17050：旧缓存路径失败，显式句柄查询成功，缓存大小不变；错误 serial、错误 owner、查询为空仍失败。
- 新增休眠其它武器 HUD 与当前枪身共存的用例。
- 调用实际生产外观管线确认 expected-HUD-missing 返回 pending，测试对象字节不变、应用计数不变。
- 完整回归、双语 DLL 与加载验证见 `build/emperor-hud-fix/VALIDATION.md`。

## 复测边界

构建基线：CS2 Client **2000899** / Patch **1.41.7.8** / Steam Build **25000182**；HLAE **2.191.1**。

新文件位于 `build/emperor-hud-fix/en` 和 `build/emperor-hud-fix/zh-CN`。不覆盖运行中的 `paint-compat-fix` DLL，不修改正式 Release，不发布 GitHub。独立刀模分析路径保持原样。

需要退出已加载旧 DLL 的 CS2 后再加载新版，沿用当前玩家的 M4A4 皇帝设置。验收必须同时包含：实际皇帝画面、日志 `hud_render_only=1`、正确 HUD 对象、世界/HUD mesh=2，以及切枪后不出现新增重复刷新。不能以字段回读和后台测试替代画面验证；如果画面仍为原皮，继续按新版日志和实际材质状态排查。
