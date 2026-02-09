#test
import logging

# 基础配置：打印到控制台，日志级别DEBUG，格式包含级别和消息
logging.basicConfig(
    level=logging.DEBUG,
    format="%(levelname)s: %(message)s"
)
logger = logging.getLogger(__name__)  # 获取当前模块的日志器（推荐用法）

name = "张三"
age = 20
logger.info("用户%s的年龄是%d", name, age)  # 占位符与参数一一对应

# 多个参数/元组传参（效果一致）
params = ("李四", 25)
logger.debug("用户%s的年龄是%d", *params)

# 字典传参（%s对应字典key，用%(key)s）
user = {"name": "王五", "age": 30}
logger.warning("用户%(name)s的年龄是%(age)d", user)





name = "吴九"
age = 50

# 1. 直接+拼接
logger.info("用户" + name + "的年龄是" + str(age))

# 2. f-string（最常用的拼接方式）
logger.warning(f"用户{name}的年龄是{age}")

# 3. 提前格式化
msg = "用户{}的年龄是{}".format(name, age)
logger.error(msg)