# Modeling PSO, DE & ES Solver

一个面向数学建模竞赛的 C++20 无梯度优化器库，适用于有界、连续、非线性问题。
目标函数和调用代码保持不变，只需修改 YAML 中的 `solver.method`，即可切换 PSO、
差分进化或演化策略算法。

## 支持的算法

| `solver.method` | 算法 |
|---|---|
| `pso` | 经典粒子群优化 |
| `de` | 基础差分进化，可选择五种变异策略 |
| `sade` | 自适应策略选择差分进化 |
| `jade` | current-to-pbest/1、外部档案和参数自适应 |
| `shade` | 成功参数历史自适应差分进化 |
| `lshade` | SHADE 加线性种群规模缩减 |
| `es` | 基础高斯演化策略，支持两种环境选择与两种重组方式 |
| `cmaes` | 标准全协方差 CMA-ES，自动学习尺度和变量相关性 |

所有算法共用以下能力：

- 从指定路径的 YAML 读取算法参数和变量边界；
- 最小化或最大化目标函数；
- 热启动初始解；
- 逐代回调和收敛历史；
- 统一的 `optimization::Result` 返回结构。

具体赛题的数学模型、数据、罚函数和结果导出应放在使用方项目中，不进入算法库。

## 目录

```text
include/optimization/    统一类型、配置和 Solver 入口
include/pso/             PSO 公共接口及旧版兼容接口
include/de/              DE 系列公共接口和参数结构
include/es/              ES 与 CMA-ES 公共接口和参数结构
src/pso.cpp              PSO 实现
src/de.cpp               DE、SaDE、JADE、SHADE、L-SHADE 实现
src/es.cpp               基础 ES、CMA-ES 和对称矩阵特征分解实现
src/solver.cpp           YAML 算法选择和统一调用分派
src/config.cpp           零第三方依赖的 YAML 子集读取器
config/optimizer.yaml    推荐使用的统一配置模板
config/pso.yaml          旧版 PSO 直接接口配置模板
tests/                   PSO、DE、ES、CMA-ES、配置和统一入口测试
docs/                    已完成赛题的接入记录
```

## 构建与测试

在 Visual Studio Developer PowerShell 中进入仓库根目录，然后执行：

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

所有路径均相对于仓库根目录；`build/` 已加入 `.gitignore`。

使用方通过 CMake 接入时，链接统一目标 `optimization::solver`：

```cmake
set(PSO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/PSO_Solver pso_solver_build EXCLUDE_FROM_ALL)
target_link_libraries(your_model PRIVATE optimization::solver)
```

## 最简单的调用方法

复制 [config/optimizer.yaml](config/optimizer.yaml) 到使用方项目，然后编写：

```cpp
#include "optimization/solver.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 3 || std::string(argv[1]) != "--config") {
        std::cerr << "Usage: your_model --config config/optimizer.yaml\n";
        return 2;
    }

    const optimization::SolverSettings settings =
        optimization::load_solver_settings(argv[2]);
    const optimization::Solver solver(settings);

    const optimization::Result result = solver.optimize(
        [](const optimization::Vector& x) {
            return x[0] * x[0] + x[1] * x[1];
        });

    std::cout << "method = " << optimization::method_name(settings.method) << '\n';
    std::cout << "best_value = " << result.best_value << '\n';
}
```

从使用方项目根目录运行：

```powershell
.\build\your_model.exe --config .\config\optimizer.yaml
```

## 选择算法

只修改一个字段即可选择求解方法：

```yaml
solver:
  # 可选：pso、de、sade、jade、shade、lshade、es、cmaes
  method: cmaes
```

选择任意 DE 变体时，还可以统一选择交叉方式：

```yaml
de_common:
  # binomial 为二项交叉，exponential 为指数交叉
  crossover: binomial
```

基础 DE 额外支持五种变异策略：

```yaml
de:
  # 可选：rand1、best1、current_to_best1、rand2、best2
  mutation_strategy: rand1
  population_size: 80
  differential_weight: 0.50
  crossover_rate: 0.90
```

`de.mutation_strategy` 只影响基础 `de`。SaDE、JADE、SHADE 和 L-SHADE 的变异及
参数自适应规则是算法自身的一部分，不使用这个开关。

基础 ES 可以选择 `(mu,lambda)` 或 `(mu+lambda)` 环境选择，以及中间或离散重组：

```yaml
es:
  parent_count: 20
  offspring_count: 80
  selection_mode: plus       # comma 或 plus
  recombination: intermediate # intermediate 或 discrete
  initial_step_size: 0.15
  adapt_step_size: true
```

CMA-ES 的种群数量和父代数量设为 `0` 时会根据问题维数自动确定，初次使用建议保留
学习率的自动设置：

```yaml
cmaes:
  population_size: 0
  parent_count: 0
  initial_step_size: 0.25
  c_sigma: 0.0  # 0 表示使用标准自动公式
  d_sigma: 0.0
  c_c: 0.0
  c1: 0.0
  c_mu: 0.0
```

各算法需要调整的参数均已在 [config/optimizer.yaml](config/optimizer.yaml) 和对应头文件
中写有中文注释：

| 参数区域 | 生效条件 | 主要内容 |
|---|---|---|
| `solver.*` | 所有算法 | 优化方向、代数、停滞条件、随机种子 |
| `pso.*` | `method: pso` | 粒子数、惯性、学习因子、限速和边界阻尼 |
| `de_common.*` | 任意 DE 变体 | 二项或指数交叉 |
| `de.*` | `method: de` | 种群、变异策略、F、CR |
| `sade.*` | `method: sade` | 种群、学习周期、F/CR 采样参数 |
| `jade.*` | `method: jade` | 种群、pbest 比例、学习率、初始参数均值 |
| `shade.*` | `method: shade` | 种群、pbest 比例、历史记忆长度和初值 |
| `lshade.*` | `method: lshade` | 初始/最小种群、pbest 比例、历史记忆参数 |
| `es.*` | `method: es` | 父子代数量、选择、重组、变异步长及 1/5 自适应 |
| `cmaes.*` | `method: cmaes` | 自动种群、全局步长、协方差学习率和数值保护 |

## 设置目标函数

目标函数不写在 YAML 中，而是在 C++ 中定义并传给 `optimize()`：

```cpp
const optimization::Objective objective = [](const optimization::Vector& x) {
    return x[0] * x[0] + x[1] * x[1];
};

const optimization::Result result = solver.optimize(objective);
```

上例求最小值，对应：

```yaml
solver:
  maximize: false
```

求最大值时设置 `maximize: true`，目标函数正常返回待最大化的值，不需要手动取负号。

`x[i]` 的含义必须与 YAML 边界顺序一致。例如 `x[0]` 是速度、`x[1]` 是角度、
`x[2]` 是时间：

```yaml
bounds:
  lower: [70.0, 0.0, 0.0]
  upper: [140.0, 6.283185307, 10.0]
```

有约束问题可以在目标函数中加入罚值。下面要求 `x[0] + x[1] <= 5`，并求最小值：

```cpp
const optimization::Objective objective = [](const optimization::Vector& x) {
    const double value = x[0] * x[0] + x[1] * x[1];
    const double excess = x[0] + x[1] - 5.0;
    const double penalty = excess > 0.0 ? 1.0e6 * excess * excess : 0.0;
    return value + penalty;  // 最小化加罚值；最大化则减去罚值
};
```

需要模型或数据时，先完成加载和预计算，再捕获模型对象：

```cpp
Model model(model_config);
const optimization::Objective objective = [&model](const optimization::Vector& x) {
    return model.evaluate(x);
};
```

不要在目标函数内部重复读取文件，因为一次求解通常会调用目标函数数千次。

## 热启动和迭代回调

`optimize()` 的第二个参数是可选初始解，第三个参数是可选逐代回调：

```cpp
const std::vector<optimization::Vector> initial_positions{
    {1.0, 2.0},
    {0.5, 1.5},
};

const auto result = solver.optimize(
    objective,
    initial_positions,
    [](std::size_t iteration, double best_value, const optimization::Vector&) {
        std::cout << iteration << ' ' << best_value << '\n';
    });
```

PSO、DE 和基础 ES 会把初始解注入种群，其余个体随机初始化；CMA-ES 会先评估所有
给定初始解，并把其中最好者作为初始均值。两种方式都允许只提供一个初始解。

## 接入新赛题

1. 定义决策向量每一维的物理意义；
2. 在 `bounds.lower`、`bounds.upper` 中按相同顺序填写边界；
3. 把数学模型封装成 `double objective(const optimization::Vector&)`；
4. 对不可行解加入方向正确的有限罚值；
5. 在 YAML 中选择算法并调用统一 `Solver`；
6. 使用多个随机种子比较最优值、稳定性和目标函数评估次数。

## 旧版 PSO 接口兼容

已有代码仍可以继续使用 `pso::load_pso_settings()`、`pso::ParticleSwarmOptimizer`、
`pso::Vector` 和 `pso::PsoResult`。新项目推荐使用统一的 `optimization::Solver`。

## YAML 支持范围

为了保证比赛现场断网也能编译，配置读取器不依赖 `yaml-cpp`。它支持本仓库所需的
缩进映射、标量、布尔值、行内数值数组和行尾注释。缺失键、重复键、类型错误、维数
不一致和非法参数都会直接报错。

## 已验证案例

A196 曾用于验证 PSO 求解器，但其业务代码已从仓库移除。建模接口、参数、结果和清理
原因记录在 [docs/A196_VALIDATION_EXAMPLE.md](docs/A196_VALIDATION_EXAMPLE.md)。
