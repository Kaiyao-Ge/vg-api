# G5 测试分轨执行冻结

日期：2026-09-03。用户要求启动一个G5子代理。
依据整改报告§9.3/§10.1、G0后续工作包表及G3/G4统筹独立复核。

## 前置条件与范围

main HEAD99ec414，G0—G4成果仍在未提交工作树，全部保留。G1/G2/G3已通过统筹复核，
Reference38/38、真实Metal72/72、关键重复240/240。G4源码与本机审查通过，Linux/Vulkan
平台验收pending。按G0“G5按后端分批解锁”，本包只实施Core/Reference/Metal及已登记的
capture临时路径治理；Vulkan测试不改，不宣称整个跨平台G5关闭。

冻结既有文件清单：

| 文件 | 工作 |
|---|---|
| tests/unit/core_test.cpp | 按Core既有职责提取测试组；区分纯Core断言与Reference plan-driven提交 |
| tests/unit/execution_plan_test.cpp | 按validation、Node/effect、access、representation、lifetime分组；语义负例留在此套件边界 |
| tests/unit/reference_raster_test.cpp | 分离纯facet/raster oracle与真实assembler驱动submit；不改变Reference oracle |
| tests/vertical_slice/metal_task_timeline_test.cpp | 19个现有mode逐项迁移，分离plan-driven与direct-adapter源文件，保留CLI路由及case集合 |
| tests/unit/capture_view_test.cpp | 仅将固定临时文件改为每次调用独占目录并安全清理；不重写capture逻辑 |
| CMakeLists.txt | 仅以上既有测试target的source/include/link登记或接入显式G5 fragment；不做G6登记框架 |

子代理是本轮中央CMake唯一编辑owner，统筹不并发修改该文件。
可新增 `cmake/g5-tests.cmake`；仅修改确有必要的 `cmake/g3-metal-tests.cmake` 测试链接，
不得改G3生产源码登记。允许的新增测试目录为 `tests/unit/core/`、
`tests/unit/execution_plan/`、`tests/unit/reference/`、`tests/vertical_slice/metal/`；
按表中职责每组一个或少量TU，必要的窄声明/fixture头放对应目录。
临时目录辅助代码限 `tests/support/scoped_temp_directory.h`（如确需共享），
文档仅新增 `docs/reports/g5-test-split.md`。

必须先在交付报告列出上述五个测试文件的函数/块/mode→分类→准确目标文件迁移表，再搬迁。
这一步是固定目录内细化owner，不是重新开展开放式整改。新增其他既有文件或生产修改先报统筹。
不要为了文件短拆成每个assert一个文件，也不要把整套逻辑挪入巨型头或#include实现文件。

## 不变量

- 不修改src/、include/vg、schema、shader、ABI、Stage0—7、capability、lifetime或Mixed-domain语义。
- plan-driven继续走真实assembler→compile→submit；不能改为harness或手工封装sealed plan。
- direct-adapter只调用G3已有窄harness或Reference已有oracle；不得新增生产入口或第二套fixture runtime。
- malformed/preflight反例归语义边界。保留所有原断言、负例、tolerance、expected bytes、返回值、skip合同。
- 原共享fixture顺序/状态依赖必须保留或显式传递；不能借拆分重置状态使原测试失去意义。
- 默认保持CTest名称、参数、属性和总数Reference38/Metal72；允许一个旧驱动链接多个职责TU。
  如必须改变可执行路由/CTest映射，先给出逐项理由报统筹，不删除原case、隐藏失败或追加skip。
- capture只处理自己原子创建的唯一临时目录；不清理共享/tmp、别人的文件或通过测试全局串行化掩盖冲突。
- 不修改Vulkan测试、启动G6、搭远程环境、提交、push、切分支或再开子代理。

## 验证与交付

动手前保存五个原测试、CTest name/command/properties映射、源码身份到独立build/g5-baseline，
不得覆盖G0/G1/G2/G3/G4基线。build目录使用build/g5-reference、g5-metal及对应OFF目录。
Debug+ASan/UBSan；测试使用独占TMPDIR，capture修复另做共享TMPDIR下多个进程并发验证。

必要门禁：

1. 明确的断言/函数体守恒或逐块对照，所有旧mode唯一归属；无生产反向测试依赖。
2. Reference38/38；非沙箱真实Metal72/72且无skip。缺沙箱MTLDevice必须升级沙箱外实测。
3. 拆分影响的core/execution-plan/reference-oracle/19个Metal mode重复运行；capture.view并发压力。
4. ON/OFF构建和公共ABI链接smoke；check_build_boundary、check_core_headers、check_compiler_sources门禁。
   Compiler比较build/g2-reference/before.bin；不重新记录golden来通过。
5. docs.check、git diff --check；不以局部绿灯替代全量或把本机结果写成Linux/Vulkan通过。

交付报告列全部修改文件、迁移映射、测试分类、精确命令/计数、守恒证据、未验证项；
完成后由统筹独立审查，不自行宣布跨平台G5或§9.3全体完成。

## 统筹独立复核（2026-09-03）

结论：本轮Core/Reference/Metal范围通过，未发现需返修的代码阻塞项。Vulkan测试拆分与
Linux/GPU门禁仍pending；未启动G6，未提交或push。本次只补充审查记录，没有修改实现。

先重新索引 `vg-api-g5-review`，再以源码核对图未覆盖的块内状态依赖。五份G5基线已独立
对照HEAD核验；Metal基线额外扣除已批准的G3 include/harness receiver迁移后与HEAD一致。
原函数体82个、Core/Reference块21个、原调用顺序与19个Metal mode守恒检查通过。
CHECK宏及失败函数、fixture状态/参数传递、ConsumeInput观察与fault helper、compiled-plan
篡改负例的原位置调用均已源码复核，未将物理harness替代真实assembler提交。

29个新增职责TU由显式fragment唯一登记，8个头自包含且没有搬入函数实现。相对G5基线，
中央CMake仅新增该fragment的include；没有生产源码或Vulkan测试变更。统筹派工前保存的
独立CTest快照与本次结果逐项一致，不仅依赖子代理自己保存的快照。

Capture目录创建使用原子create_directory并处理碰撞，构造成功后才取得清理职责；对象
不可复制，只清理自己创建的目录。保留原CLI和105项断言，未通过全局串行化规避并发。

| 统筹亲自复跑 | 结果 |
|---|---|
| dev-reference / dev-metal构建 | 成功，Debug + ASan/UBSan |
| Reference全量 | 38/38，20.04秒，无skip |
| 非沙箱真实Metal全量 | 72/72，36.74秒，无skip |
| 受影响22项各重复20次 | 440/440，67.44秒，无skip |
| Capture并发（使用统筹重建的dev-reference可执行文件） | 共享TMPDIR、8 worker共64进程，64/64；无残留，无关哨兵保留 |
| G5头/owner及原测试守恒 | 8个头、82函数、21块、原断言与CTest映射全部通过 |
| ON/OFF边界与OFF重建 | Reference41/Metal52生产TU；OFF均0测试；CTest映射摘要仍与G0一致 |
| OFF probe / 公共ABI smoke | Reference及Apple M1通过，128×128 PPM；不是Vulkan验证 |
| G1/G2显式门禁 | 64类型/1409实现行、16头；Compiler24567字节与原baseline一致 |
| G4守恒复核 | 64函数/19类型通过，仅源码证据 |
| 生产依赖/文档/diff | 无tests反向include、Metal生产库无vg::tests/AdapterHarness；docs及diff检查通过 |

全量和压力日志：`build/g5-baseline/parent-reference.log`、`parent-metal.log`、
`parent-pressure.log`。使用交付报告§5相同命令，将ON目录替换为build/dev-reference与
build/dev-metal；OFF目录仍为build/g5-off-reference与build/g5-off-metal。每轮完整CTest
使用新的独占TMPDIR，capture专项则刻意共享TMPDIR。三条既有外部文档来源warning未变。
