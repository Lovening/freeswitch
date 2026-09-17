# mod_java 使用说明

`mod_java` 将 FreeSWITCH 的 C API 通过 SWIG 暴露给 Java。它允许在 dialplan 中执行 Java 类来处理当前通话。

## 1. 配置 JVM

在 `conf/autoload_configs/java.conf.xml` 中指定 JVM 动态库和 Java classpath：

```xml
<configuration name="java.conf" description="Java Plug-Ins">
  <javavm path="/opt/jdk1.8.0/jre/lib/amd64/server/libjvm.so"/>
  <options>
    <option value="-Djava.class.path=$${script_dir}/freeswitch.jar:$${script_dir}/myapp.jar"/>
  </options>
</configuration>
```

将 `javavm` 的 `path` 替换为服务器上实际的 `libjvm.so` 路径。`myapp.jar` 是业务代码的 JAR 文件，通常放在 `$${script_dir}` 对应的脚本目录。

## 2. 编写 Java 呼叫处理类

Java 类必须有无参构造函数，并提供下面签名的 `run` 方法。实现 `FreeswitchScript` 接口可让约束更明确。

```java
package com.example;

import org.freeswitch.FreeswitchScript;
import org.freeswitch.swig.CoreSession;
import org.freeswitch.swig.freeswitch;

public class MyCallHandler implements FreeswitchScript {
    public MyCallHandler() {
    }

    @Override
    public void run(String uuid, String args) {
        CoreSession session = new CoreSession(uuid);

        freeswitch.console_log("info", "Java handler started: " + args + "\n");
        session.answer();
        session.streamFile("ivr/ivr-welcome_to_freeswitch.wav", "");
        session.hangup("NORMAL_CLEARING");
    }
}
```

`uuid` 是当前 FreeSWITCH session 的 UUID，`args` 是 dialplan 的 `data` 中类名之后的所有参数。

## 3. 编译和打包

编译时需要引用 `freeswitch.jar`：

```bash
javac -cp /path/to/freeswitch.jar -d build src/com/example/MyCallHandler.java
jar cf myapp.jar -C build .
cp myapp.jar /usr/local/freeswitch/scripts/
```

路径应与 `java.conf.xml` 内的 `-Djava.class.path` 保持一致。

## 4. 在 dialplan 中调用

```xml
<extension name="java_demo">
  <condition field="destination_number" expression="^9999$">
    <action application="java" data="com.example.MyCallHandler hello world"/>
  </condition>
</extension>
```

`data` 的格式为：

```text
完全限定类名 参数1 参数2 ...
```

调用流程为：

```text
dialplan 的 java application
  -> mod_java 的 java_function
  -> org.freeswitch.Launcher.launch(uuid, data)
  -> 反射创建业务类实例
  -> 业务类 run(uuid, args)
```

## 5. 加载模块

确认 `mod_java` 已编译、已加入 `modules.conf` 后，在 FS CLI 中执行：

```text
load mod_java
```

修改 `java.conf.xml` 或替换 JAR 后，可执行：

```text
reload mod_java
```

使用当前环境的 CLI 端口连接：

```bash
fs_cli -P 8121
```
