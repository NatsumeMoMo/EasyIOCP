# High Performance IOCP Server

This project code is an improvement based on the following two projects: https://github.com/TTGuoying/IOCPServer and https://github.com/loui4939/IOCPServer. The first project established the basic framework, and the second project added support for the x64 platform and fixed some bugs.

The main contributions of this project are:

- Significantly improved the data transmission functionality, which was incomplete in the previous two projects. This project introduces code for a send queue, which, after testing and adjustments, enables the server to support high-speed, high-volume data transmission without crashing. The server code can be used directly; you only need to override the relevant virtual functions to implement your business logic without worrying about the network layer code.
- Made minor adjustments to some server details.

------

http://www.cnblogs.com/tanguoying/p/8439701.html

https://blog.csdn.net/piggyxp/article/details/6922277

These two articles can help you understand the detailed working principles of IOCP (Chinese articles, you may need to translate them).
