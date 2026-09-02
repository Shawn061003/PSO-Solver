# A196：PSO 使用范例与仓库边界反例

## 1. 文档定位

2025 年全国大学生数学建模竞赛 A 题优秀论文 A196 的第二问，曾用于验证本仓库的
PSO 求解器。

它有两层意义：

- 作为使用范例，说明怎样把具体数学模型封装成 PSO 的目标函数；
- 作为仓库边界的反例，说明赛题运动学、几何判定和结果导出不应长期混入通用算法库。

本文件只保留接入方法和验证记录，不参与编译，也不提供 A196 的可执行实现。下面的
`A196Model`、`A196ModelConfig` 均代表使用方项目中的业务代码，不属于 PSO 库。

## 2. A196 如何转化为 PSO 问题

### 2.1 决策变量顺序

第二问采用四维决策向量：

```text
x[0] = 无人机水平航向角 theta，单位 rad
x[1] = 无人机速度 v，单位 m/s
x[2] = 烟幕弹投放时刻 t_drop，单位 s
x[3] = 投放至起爆的延迟 tau，单位 s
```

这个顺序必须在以下四处完全一致：

1. YAML 中 `bounds.lower` 的元素顺序；
2. YAML 中 `bounds.upper` 的元素顺序；
3. 目标函数从 `pso::Vector` 取值的顺序；
4. 最终结果中对 `best_position` 的解释顺序。

一旦其中一处顺序不同，PSO 仍然可能正常运行，但结果会被解释成错误的物理量，因此
比赛时建议给每一维写出类似上面的映射表。

### 2.2 目标函数

A196 模型先根据决策变量计算无人机、烟幕弹、云团和导弹的位置，再判断导弹到
真实圆柱上下底面圆周各离散点的视线是否与烟幕球相交。

某时刻只有在全部视线均被截断时才记为有效遮蔽。目标函数返回起爆后 20 秒有效期
内的总遮蔽时长：

```text
输入：theta、v、t_drop、tau
  ↓
检查速度、时间和起爆高度等约束
  ↓
计算投放点和起爆点
  ↓
沿时间轴计算导弹位置与烟幕云团位置
  ↓
逐时刻判断圆柱上下圆周是否被完全遮蔽
  ↓
返回所有有效遮蔽区间的总长度
```

因为目标是“遮蔽时间尽可能长”，所以配置中必须令 `maximize: true`。

## 3. 推荐的使用方项目结构

通用 PSO 仓库和 A196 模型应保持分离。可以把 PSO 仓库作为 Git 子模块或普通源码
目录放入使用方项目：

```text
a196-practice/
├── CMakeLists.txt
├── config/
│   └── a196_pso.yaml
├── src/
│   ├── main.cpp
│   ├── a196_model.cpp
│   └── a196_model.hpp
└── third_party/
    └── PSO_Solver/
```

这里：

- `third_party/PSO_Solver` 只负责搜索决策向量；
- `a196_model.*` 负责运动学、几何遮蔽、约束和遮蔽时长计算；
- `main.cpp` 负责读取 YAML、连接模型与 PSO、输出结果。

## 4. 第一步：在 CMake 中引入求解器

使用方的 `CMakeLists.txt` 可以这样写：

```cmake
cmake_minimum_required(VERSION 3.20)
project(a196_practice LANGUAGES CXX)

# 将通用求解器作为子目录加入，但不构建它自己的测试目标。
set(PSO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(
    third_party/PSO_Solver
    pso_solver_build
    EXCLUDE_FROM_ALL
)

add_executable(a196_solver
    src/main.cpp
    src/a196_model.cpp
)
target_compile_features(a196_solver PRIVATE cxx_std_20)
target_link_libraries(a196_solver PRIVATE pso::solver)

if(MSVC)
    target_compile_options(a196_solver PRIVATE /W4 /permissive- /utf-8)
endif()
```

关键点是链接公开目标 `pso::solver`。使用方不应复制 `src/pso.cpp`，也不应直接依赖
求解器内部实现文件。

## 5. 第二步：准备 YAML 文件

下面是验证时使用的完整 PSO 部分。文件路径可以任意，但运行程序时必须通过
`--config` 明确指定：

```yaml
solver:
  swarm_size: 72
  max_iterations: 180
  inertia_start: 0.90
  inertia_end: 0.40
  cognitive: 1.70
  social: 1.70
  velocity_limit_ratio: 0.20
  boundary_damping: 0.50
  tolerance: 1.0e-7
  stall_iterations: 50
  seed: 2026
  maximize: true

# 顺序固定为 theta、v、t_drop、tau。
bounds:
  lower: [0.0, 70.0, 0.0, 0.0]
  upper: [6.283185307179586, 140.0, 65.0, 65.0]

# 以下字段属于 A196Model，由使用方用 YamlConfig 自行读取。
a196:
  circle_samples: 300
  scan_step: 0.10
  boundary_tolerance: 1.0e-5
  cloud_lifetime: 20.0
  cloud_radius: 10.0
```

`load_pso_settings()` 只读取 `solver.*` 和 `bounds.*`，会忽略额外的 `a196.*`。因此同一
个 YAML 可以同时保存通用算法参数和具体模型参数，二者仍由不同模块负责解释。

## 6. 第三步：为模型定义一个稳定接口

建议让 A196 模型对外只暴露“检查可行性”和“计算目标值”，不要让 PSO 主程序了解
几何计算细节。概念接口如下：

```cpp
struct A196Decision {
    double heading;
    double speed;
    double drop_time;
    double detonation_delay;
};

struct A196ModelConfig {
    std::size_t circle_samples;
    double scan_step;
    double boundary_tolerance;
    double cloud_lifetime;
    double cloud_radius;
};

class A196Model {
public:
    explicit A196Model(A196ModelConfig config);

    bool feasible(const A196Decision& decision) const;
    double coverage_duration(const A196Decision& decision) const;
};
```

实际实现中，圆柱离散点等不随粒子变化的数据应在 `A196Model` 构造时预计算。不要在
每次目标函数调用时重新读 YAML、重新生成固定点集或打开结果文件，否则数千次函数
评估会把大量时间耗费在重复准备工作上。

## 7. 第四步：从指定路径读取全部参数

使用方主程序应同时读取通用设置和模型专用设置：

```cpp
const std::filesystem::path config_path = argv[2];

// 通用求解器只读取 solver.* 与 bounds.*。
const pso::PsoSettings settings = pso::load_pso_settings(config_path);

// 使用方可以再次读取同一文件中的 a196.*。
const pso::YamlConfig yaml = pso::YamlConfig::load(config_path);
A196ModelConfig model_config{
    .circle_samples = yaml.get_size("a196.circle_samples"),
    .scan_step = yaml.get_double("a196.scan_step"),
    .boundary_tolerance = yaml.get_double("a196.boundary_tolerance"),
    .cloud_lifetime = yaml.get_double("a196.cloud_lifetime"),
    .cloud_radius = yaml.get_double("a196.cloud_radius"),
};
```

在创建优化器前应确认问题维数：

```cpp
if (settings.bounds.size() != 4) {
    throw std::runtime_error("A196 第二问必须配置四维边界");
}
if (!settings.solver.maximize) {
    throw std::runtime_error("A196 第二问必须设置 solver.maximize: true");
}
```

## 8. 第五步：把 `pso::Vector` 转换为物理变量

不要在复杂目标函数中到处直接使用 `x[0]`、`x[1]`。应在入口处立即转换成有名称的
结构体：

```cpp
A196Decision decode_decision(const pso::Vector& x) {
    if (x.size() != 4) {
        throw std::invalid_argument("A196 决策向量维数必须为 4");
    }
    return {
        .heading = x[0],
        .speed = x[1],
        .drop_time = x[2],
        .detonation_delay = x[3],
    };
}
```

这样可以避免在运动方程中把投放时刻和起爆延迟下标写反。

## 9. 第六步：封装目标函数和罚值

创建模型和优化器：

```cpp
const A196Model model(model_config);
const pso::ParticleSwarmOptimizer optimizer(
    settings.bounds, settings.solver);
```

然后将模型包装成 PSO 接受的目标函数：

```cpp
const pso::Objective objective = [&model](const pso::Vector& x) {
    const A196Decision decision = decode_decision(x);

    if (!model.feasible(decision)) {
        // 遮蔽时长一定非负，因此 0 可以作为最大化问题的不可行解罚值。
        return 0.0;
    }
    return model.coverage_duration(decision);
};
```

罚值的方向必须与优化方向匹配：

- 最大化非负指标时，不可行解可以返回 `0.0` 或更小值；
- 最小化非负代价时，不可行解应返回一个足够大的正值；
- 不要返回 `NaN`，求解器会将其视为模型错误并停止；
- 如果“零遮蔽”本身是合法解，可返回负罚值以便与合法零值区分。

## 10. 第七步：按需加入物理启发式初始粒子

A196 的严格遮蔽条件会让可行区域比较窄。如果全部随机粒子初始目标值都是零，PSO
缺少可利用的方向信息。可以加入少量基于物理常识的热启动点，同时保留多数随机粒子：

```cpp
std::vector<pso::Vector> initial_positions{
    // theta, speed, t_drop, tau
    {0.00, 140.0, 0.5, 0.10},
    {0.05, 135.0, 1.0, 0.15},
    {0.10, 130.0, 1.5, 0.20},
};
```

它们只是初始粒子，不是固定答案。若 `swarm_size=72`，上面三个点占前三个粒子，剩余
69 个粒子仍由求解器在完整边界内随机生成。热启动粒子若轻微越界，会被裁剪到边界。

## 11. 第八步：运行 PSO 并观察收敛

完整调用如下：

```cpp
const pso::PsoResult result = optimizer.optimize(
    objective,
    initial_positions,
    [](std::size_t iteration,
       double best_value,
       const pso::Vector& best_position) {
        if (iteration == 1 || iteration % 10 == 0) {
            std::cout << "iteration=" << iteration
                      << ", best duration=" << best_value << " s"
                      << ", heading=" << best_position[0] << '\n';
        }
    });
```

回调在每一代全部粒子更新完成后执行。回调适合打印或记录数据，不应在其中修改模型、
边界或目标函数。

## 12. 第九步：解释并保存结果

求解结束后再次按约定顺序解释最优向量：

```cpp
const A196Decision best = decode_decision(result.best_position);

std::cout << "heading = " << best.heading << " rad\n"
          << "speed = " << best.speed << " m/s\n"
          << "drop time = " << best.drop_time << " s\n"
          << "detonation delay = " << best.detonation_delay << " s\n"
          << "covered duration = " << result.best_value << " s\n"
          << "iterations = " << result.iterations << '\n'
          << "evaluations = " << result.evaluations << '\n'
          << "elapsed = " << result.elapsed_seconds << " s\n";
```

`result.converged=true` 只表示连续 `stall_iterations` 代没有超过 `tolerance` 的改善，
不代表数学上已经证明全局最优。因此正式论文应至少更换多个随机种子重复运行，并比较
最优值分布。

## 13. 第十步：通过指定 YAML 路径运行

主程序应强制要求 `--config`，避免比赛时误用编译进代码的旧参数：

```cpp
int main(int argc, char* argv[]) {
    if (argc != 3 || std::string(argv[1]) != "--config") {
        std::cerr << "Usage: a196_solver --config path/to/a196_pso.yaml\n";
        return 2;
    }

    // 后续使用 argv[2] 调用 load_pso_settings() 和 YamlConfig::load()。
}
```

调用示例：

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\a196_solver.exe --config .\config\a196_pso.yaml
```

上述命令应在 `a196-practice` 项目根目录执行。源码目录、构建目录和 YAML 都使用相对
路径，因此整个项目移动到其他磁盘或设备后不需要修改命令。若使用多配置生成器，程序
可能位于 `build\Release\a196_solver.exe`，此时只需相应调整可执行文件的相对路径。

建议在结果文件中同时保存 YAML 原文、随机种子、最优向量、最优值、迭代次数和目标函数
评估次数。仅保存最终四个参数，会失去实验可复现性。

## 14. 验证结果

固定随机种子 2026 的一次 Release 求解结果为：

| 量 | 数值 |
|---|---:|
| 航向角 | 0.094326 rad / 5.404491 deg |
| 无人机速度 | 139.865441 m/s |
| 投放时刻 | 0.556146 s |
| 起爆延迟 | 0.338064 s |
| 起爆时刻 | 0.894210 s |
| 有效遮蔽区间 | [0.894515, 5.482497] s |
| 有效遮蔽时长 | 4.587982 s |

该结果与论文报告的约 4.5873 秒一致。验证期间还进行了两项回归检查：

- 第一问固定策略得到约 1.391643 秒；
- 将论文公布的第二问策略代回模型得到约 4.5873 秒。

这些结果说明 PSO 的最大化、边界处理、固定随机种子、初始粒子注入和收敛记录接口
可以用于真实建模问题。

## 15. 为什么业务代码必须移出仓库

早期验证版本曾把以下内容直接放入 PSO 仓库：

- 导弹和无人机初始坐标；
- 抛体与烟幕下沉方程；
- 圆柱目标离散化；
- 视线与烟幕球相交判定；
- A196 专用命令行程序、YAML 和 JSON 输出。

这些代码证明算法可用，但会让通用库承担赛题业务职责。后续赛题一旦改变目标形状、
约束或变量维度，就需要改动库本身，既不利于复用，也容易在比赛中引入旧题常量。

正确边界是：

```text
本仓库：PSO + 通用 YAML 参数加载
使用方：具体模型 + 罚函数 + 数据读取 + 结果解释
```

因此 A196 源码、配置、测试和生成结果已全部移除，只留下本验证文档作为参考。
