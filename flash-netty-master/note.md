<h1 align="center">跟着闪电侠学netty笔记</h1>

## netty使用
### ServerBootstrap
+ handler在初始化时就会执行
+ childHandler会在客户端成功connect后才执行，这是两者的区别。

### 一个链接到handler对应关系:
+ connection -1:1- channle -1:1-channelPipeline -1:n- channelHandlerContext -1:n- handler
+ channelPipeline 双向联表维护多个channelHandlerContext,channelHandlerContext为链表中的每一个节点
+ 执行顺序,实现ChannelInboundHandler接口的类按照添加顺序执行
+ 实现ChannelOutboundHandler接口的类按照添加倒序执行

### 数据封装byteBuffer
+ 通过指针读取写入数据
+ 使用堆外内存要手动释放内存,byteBuffer释放逻辑是引用计数法,要根据引用次数进行多次释放,避免内存逸出
+ 尽量使用netty本身提供byteBuffer相关的api其会自动释放
  >例:实现ByteToMessageDecoder类中decode方法进行转换会自动释放内存

### 自定义逻辑处理(ChannelHandler)
+ netty提供的ChannelHandler
``` 
    ChannelOutboundHandlerAdapter优点:默认实现了顶层接口方法
    ChannelInboundHandlerAdapter优点:默认实现了顶层接口方法
        |
    SimpleChannelInboundHandler优点:通过子类泛型判断传入的数据类型是否与其匹配从而决定是否执行子类逻辑
    自定义ChannelHandler继承SimpleChannelInboundHandler重写核心方法实现业务逻辑
```


### 拆包/粘包
+ 客户单与服务器端之间以字节的形式传输数据,如果不按照约定的协议进行拆包则会出现数据不完整或冗余数据;
+ 拆包器:根据我们的自定义协议，把数据拼装成一个个符合自定义数据包大小的ByteBuf，然后发送到自定义协议的解码器中去解码;
+ Netty提供的拆包器,LengthFieldBasedFrameDecoder保证字节数据完整性;

### ChannelHandler
+ 生命周期:生命周期划分详细,方便扩展
+ 热插拔:ChannelHandler执行逻辑成功后在之后的通信中不需要再执行,则可以在逻辑执行后调用handlerRemoved方法删除此ChannelHandler

### ChannelGroup
+ 批量操作Channel

### netty性能优化
+ 减少建立连接时创建channelHandler实例数量
+ 无状态channelHandler单例,避免每个新链接都创建,用@ChannelHandler.Sharable注解表名共享
+ 压缩channelHandler数量,编码和解码逻辑可以放在MessageToMessageCodec子类中实现,减少channelHandle数量
+ 更改事件传播源
```
  ctx.writeAndFlush() 从当前节点向前找到合适的ChannelOutboundHandler执行
  ctx.channel().writeAndFlush()先找到最后一个ChannelOutboundHandler再向前找
 ```
+ 减少阻塞主线程的操作
netty会开启cpu核数的2倍管理成千上万个连接,如果某个连接中业务逻辑有费时操作应异步处理否则会拖慢整个系统吞吐量
+ 统计业务逻辑执行时间
writeAndFlush是异步操作,要添加监听器记录结束时间
+ 空闲检查和心跳检查
```
  假死:tcp层连接中断但应用程序没有捕获到连接一致占用,导致服务端资源被占用,客户端发送消息失败
  空闲检查:多长时间内没有读写的Channel关闭
  心跳检查:定期发送心跳数据,防止空闲检查误操作,一般空闲检查时间是心跳时间为的两倍以上防止网络抖动误操作
```
=========================================  
## netty源码
### 服务端启动流程解析
1. 创建channel
  + 核心组件: 
    + ChannelId是Netty中每条Channel的唯一标识，类似Snowflake算法，通过机器号、进程号、时间戳、随机数等方式生成。
    + Unsafe (略)
    + ChannelPipeline ，它包含了一个ChannelHandler链表，用于处理或者拦截Channel的Inbound事件和outbound操作。
  + 初始化服务端Channel
    + 设置Channel的Option与Attr
  + 添加用户自定义的处理逻辑到启动流程和一个特殊的处理逻辑(ServerBootstrapAcceptor)
+ 注册channel(AbstractBootstrap.java)
  + 就是把前面创建的JDK的Channel注册到Selector，并且把Netty领域的Channel当作一个attachment绑定上去，同时回调handlerAdded和channelRegistered事件。
+ 绑定端口
  + AbstractBootstrap#doBind

2. Reactor线程模型解析
+ NioEventLoopGroup的创建
  1. 在默认情况下，NioEventLoopGroup会创建两倍CPU核数个NioEventLoop，一个NioEventLoop和一个Selector及一个MPSC任务队列一一对应。
  2. NioEventLoop线程的命名规则是nioEventLoopGroup-xx-yy，xx表示全局第xx个NioEventLoopGroup，yy表示这个NioEventLoop在NioEventLoopGroup中是第yy个。
  3. 线程选择器的作用是为一个连接选择一个NioEventLoop，如果NioEventLoop的个数为2的幂，则Netty会使用与运算进行优化。
+ NioEventLoop对应线程的创建和启动
+ NioEventLoop的执行流程
  + 执行一次轮询
    + IO事件主要包含新连接接入事件和连接的数据读写事件
    + 不断地轮询是否有IO事件发生，并且在轮询过程中不断检查是否有任务需要执行，保证Netty任务队列中的任务能够及时执行，轮询过程使用一个计数器避开了JDK的空轮询Bug，整个过程还是比较清晰的。
  +处理产生IO事件的channel
    1. Netty使用数组替换JDK原生的HashSet来提升处理IO事件的效率。
    2. 每个SelectionKey上都绑定了Netty类AbstractChannel对象作为attachment，在处理每个SelectionKey的时候，都可以找到AbstractChannel，然后通过Pipeline将处理串行到ChannelHandler，回调到用户方法。
    添加任务

## TODO