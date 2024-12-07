#include "IOCPBase.h"
#include <mstcpip.h>

#pragma comment(lib, "WS2_32.lib")

IOContextPool SocketContext::ioContextPool;		// 初始化

IOCPBase::IOCPBase() :
	completionPort(INVALID_HANDLE_VALUE),
	workerThreads(NULL),
	workerThreadNum(2),
	listenSockContext(NULL),
	fnAcceptEx(NULL),
	fnGetAcceptExSockAddrs(NULL),
	connectCnt(0),
	acceptPostCnt(0)
{
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

#include <iostream>
IOCPBase::~IOCPBase()
{
	Stop();
	WSACleanup();
}


BOOL IOCPBase::Start(std::string ip, int port)
{
	this->IP = ip;

	this->port = port;

	if (false == InitializeIOCP())
		return false;

	if (false == InitializeListenSocket())
	{
		DeInitialize();
		return false;
	}

	return true;
}

void IOCPBase::Stop()
{
	if (listenSockContext != NULL && listenSockContext->connSocket != INVALID_SOCKET)
	{
		// 激活关闭事件
		SetEvent(stopEvent);

		for (int i = 0; i < workerThreadNum; i++)
		{
			// 通知所有完成端口退出
			PostQueuedCompletionStatus(completionPort, 0, EXIT_CODE, NULL);
		}

		// 等待所有工作线程退出
		WaitForMultipleObjects(workerThreadNum, workerThreads, TRUE, INFINITE);

		// 释放其他资源
		DeInitialize();
	}
}

BOOL IOCPBase::SendData(SocketContext* socketContext, char* data, int size)
{
	if (socketContext == NULL || data == NULL || size <= 0)
		return FALSE;

	// 将数据添加到发送队列
	socketContext->EnqueueSendData(data, size);

	// 尝试启动发送操作
	if (socketContext->TryStartSend())
	{
		// 获取发送的数据
		std::vector<char> sendData;
		if (socketContext->GetNextSendData(sendData))
		{
			// 获取一个发送的 IOContext
			IOContext* ioContext = socketContext->GetNewIOContext();
			if (ioContext == nullptr)
				return FALSE;

			// 复制数据到发送缓冲区
			memcpy(ioContext->wsaBuf.buf, sendData.data(), sendData.size());
			ioContext->wsaBuf.len = static_cast<ULONG>(sendData.size());
			ioContext->ioType = SEND_POSTED;
			ioContext->ioSocket = socketContext->connSocket;

			// 投递发送请求
			if (!PostSend(socketContext, ioContext))
			{
				socketContext->RemoveContext(ioContext);
				return FALSE;
			}
		}
	}




	return TRUE;
}

std::string IOCPBase::GetLocalIP()
{
	return std::string(DEFAULT_IP);
}

BOOL IOCPBase::InitializeIOCP()
{
	completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (NULL == completionPort)
	{
		return false;
	}

	workerThreadNum = WORKER_THREADS_PER_PROCESSOR * GetNumOfProcessors();
	workerThreads = new HANDLE[workerThreadNum];

	for (int i = 0; i < workerThreadNum; i++)
	{
		workerThreads[i] = CreateThread(0, 0, WorkerThreadProc, (void*)this, 0, 0);
	}
	return true;
}

BOOL IOCPBase::InitializeListenSocket()
{
	// 生成用于监听的socket的Context
	listenSockContext = new SocketContext;
	listenSockContext->connSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == listenSockContext->connSocket)
		return false;

	// 将socket绑定到完成端口中
	if (NULL == CreateIoCompletionPort((HANDLE)listenSockContext->connSocket, completionPort, (ULONG_PTR)listenSockContext, 0))
	{
		RELEASE_SOCKET(listenSockContext->connSocket);
		return false;
	}

	//服务器地址信息，用于绑定socket
	sockaddr_in serverAddr;

	// 填充地址信息
	ZeroMemory((char*)&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(port);

	// 绑定地址和端口
	if (SOCKET_ERROR == bind(listenSockContext->connSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)))
	{
		return false;
	}

	// 开始监听
	if (SOCKET_ERROR == listen(listenSockContext->connSocket, SOMAXCONN))
	{
		return false;
	}

	GUID guidAcceptEx = WSAID_ACCEPTEX;
	GUID guidGetAcceptSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
	// 提取扩展函数指针
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		listenSockContext->connSocket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidAcceptEx,
		sizeof(guidAcceptEx),
		&fnAcceptEx,
		sizeof(fnAcceptEx),
		&dwBytes,
		NULL,
		NULL))
	{
		DeInitialize();
		return false;
	}

	if (SOCKET_ERROR == WSAIoctl(
		listenSockContext->connSocket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidGetAcceptSockAddrs,
		sizeof(guidGetAcceptSockAddrs),
		&fnGetAcceptExSockAddrs,
		sizeof(fnGetAcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL))
	{
		DeInitialize();
		return false;
	}

	for (size_t i = 0; i < MAX_POST_ACCEPT; i++)
	{
		IOContext* ioContext = listenSockContext->GetNewIOContext();
		if (false == PostAccept(listenSockContext, ioContext))
		{
			listenSockContext->RemoveContext(ioContext);
			return false;
		}
	}
	return true;
}

void IOCPBase::DeInitialize()
{
	//删除关事件句柄
	RELEASE_HANDLE(stopEvent);

	// 释放工作者线程句柄指针
	for (int i = 0; i < workerThreadNum; i++)
	{
		RELEASE_HANDLE(workerThreads[i]);
	}

	RELEASE(workerThreads);

	// 关闭IOCP句柄
	RELEASE_HANDLE(completionPort);

	// 关闭监听Socket
	RELEASE(listenSockContext);
}

BOOL IOCPBase::IsSocketAlive(SOCKET sock)
{
	int nByteSent = send(sock, "", 0, 0);
	if (-1 == nByteSent)
		return false;
	return true;
}

int IOCPBase::GetNumOfProcessors()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwNumberOfProcessors;
}

BOOL IOCPBase::AssociateWithIOCP(SocketContext* sockContext)
{
	// 将用于和客户端通信的SOCKET绑定到完成端口中
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)sockContext->connSocket, completionPort, (ULONG_PTR)sockContext, 0);

	if (NULL == hTemp)
	{
		return false;
	}

	return true;
}

BOOL IOCPBase::PostAccept(SocketContext* sockContext, IOContext* ioContext)
{
	DWORD dwBytes = 0;
	ioContext->ioType = ACCEPT_POSTED;
	ioContext->ioSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == ioContext->ioSocket)
	{
		return false;
	}

	// 将接收缓冲置为0,令AcceptEx直接返回,防止拒绝服务攻击
	if (false == fnAcceptEx(listenSockContext->connSocket, ioContext->ioSocket, ioContext->wsaBuf.buf, 0, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &dwBytes, &ioContext->overLapped))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			return false;
		}
	}

	InterlockedIncrement(&acceptPostCnt);
	return true;
}

BOOL IOCPBase::PostRecv(SocketContext* sockContext, IOContext* ioContext)
{
	DWORD dwFlags = 0, dwBytes = 0;
	ioContext->Reset();
	ioContext->ioType = RECV_POSTED;

	int nBytesRecv = WSARecv(ioContext->ioSocket, &ioContext->wsaBuf, 1, &dwBytes, &dwFlags, &ioContext->overLapped, NULL);
	// 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		DoClose(sockContext);
		return false;
	}
	return true;
}

BOOL IOCPBase::PostSend(SocketContext* sockContext, IOContext* ioContext)
{
	ioContext->ioType = SEND_POSTED;
	DWORD dwBytes = 0, dwFlags = 0;

	// 确保发送缓冲区大小不超过 BUFF_SIZE
	if (ioContext->wsaBuf.len > BUFF_SIZE)
		ioContext->wsaBuf.len = BUFF_SIZE;

	if (::WSASend(ioContext->ioSocket, &ioContext->wsaBuf, 1, &dwBytes, dwFlags, &ioContext->overLapped, NULL) != NO_ERROR)
	{
		if (WSAGetLastError() != WSA_IO_PENDING)
		{
			DoClose(sockContext);
			return false;
		}
	}
	return true;
}


BOOL IOCPBase::DoAccpet(SocketContext* sockContext, IOContext* ioContext)
{

	InterlockedIncrement(&connectCnt);
	InterlockedDecrement(&acceptPostCnt);
	SOCKADDR_IN* clientAddr = NULL;
	SOCKADDR_IN* localAddr = NULL;
	int clientAddrLen, localAddrLen;
	clientAddrLen = localAddrLen = sizeof(SOCKADDR_IN);

	// 1. 获取地址信息 （GetAcceptExSockAddrs函数不仅可以获取地址信息，还可以顺便取出第一组数据）
	fnGetAcceptExSockAddrs(ioContext->wsaBuf.buf, 0, localAddrLen, clientAddrLen, (LPSOCKADDR*)&localAddr, &localAddrLen, (LPSOCKADDR*)&clientAddr, &clientAddrLen);

	// 2. 为新连接建立一个SocketContext 
	SocketContext* newSockContext = new SocketContext;
	newSockContext->connSocket = ioContext->ioSocket;
	memcpy_s(&(newSockContext->clientAddr), sizeof(SOCKADDR_IN), clientAddr, sizeof(SOCKADDR_IN));

	// 3. 将listenSocketContext的IOContext 重置后继续投递AcceptEx
	ioContext->Reset();
	if (false == PostAccept(listenSockContext, ioContext))
	{
		listenSockContext->RemoveContext(ioContext);
	}

	// 4. 将新socket和完成端口绑定
	if (NULL == CreateIoCompletionPort((HANDLE)newSockContext->connSocket, completionPort, (ULONG_PTR)newSockContext, 0))
	{
		DWORD dwErr = WSAGetLastError();
		if (dwErr != ERROR_INVALID_PARAMETER)
		{
			DoClose(newSockContext);
			return false;
		}
	}

	//// 并设置tcp_keepalive
	//tcp_keepalive alive_in;
	//tcp_keepalive alive_out;
	//alive_in.onoff = TRUE;
	//alive_in.keepalivetime = 1000 * 60;  // 60s  多长时间（ ms ）没有数据就开始 send 心跳包
	//alive_in.keepaliveinterval = 1000 * 10; //10s  每隔多长时间（ ms ） send 一个心跳包
	//unsigned long ulBytesReturn = 0;
	//if (SOCKET_ERROR == WSAIoctl(newSockContext->connSocket, SIO_KEEPALIVE_VALS, &alive_in, sizeof(alive_in), &alive_out, sizeof(alive_out), &ulBytesReturn, NULL, NULL))
	//{
	//	TRACE(L"WSAIoctl failed: %d/n", WSAGetLastError());
	//}


	OnConnectionEstablished(newSockContext);

	// 5. 建立recv操作所需的ioContext，在新连接的socket上投递recv请求
	IOContext* newIoContext = newSockContext->GetNewIOContext();
	newIoContext->ioType = RECV_POSTED;
	newIoContext->ioSocket = newSockContext->connSocket;
	// 投递recv请求
	if (false == PostRecv(newSockContext, newIoContext))
	{
		DoClose(sockContext);
		return false;
	}

	return true;
}

BOOL IOCPBase::DoRecv(SocketContext* sockContext, IOContext* ioContext)
{
	OnRecvCompleted(sockContext, ioContext);
	ioContext->Reset();
	if (false == PostRecv(sockContext, ioContext))
	{
		DoClose(sockContext);
		return false;
	}
	return true;
}

BOOL IOCPBase::DoSend(SocketContext* sockContext, IOContext* ioContext)
{

	// 调用回调函数处理发送完成
	OnSendCompleted(sockContext, ioContext);


	// 释放当前的 IOContext
	sockContext->RemoveContext(ioContext);

	// 标记发送操作完成
	sockContext->FinishSend();

	// 检查并发送下一个数据块
	std::vector<char> nextData;
	if (sockContext->GetNextSendData(nextData))
	{
		// 获取一个发送的 IOContext
		IOContext* newIoContext = sockContext->GetNewIOContext();
		if (newIoContext != nullptr)
		{
			// 复制数据到发送缓冲区
			memcpy(newIoContext->wsaBuf.buf, nextData.data(), nextData.size());
			newIoContext->wsaBuf.len = static_cast<ULONG>(nextData.size());
			newIoContext->ioType = SEND_POSTED;
			newIoContext->ioSocket = sockContext->connSocket;

			// 投递发送请求
			if (!PostSend(sockContext, newIoContext))
			{
				sockContext->RemoveContext(newIoContext);
				DoClose(sockContext);
				return FALSE;
			}
		}
	}

	return TRUE;

}

BOOL IOCPBase::DoClose(SocketContext* sockContext)
{
	InterlockedDecrement(&connectCnt);
	RELEASE(sockContext);
	return true;
}

DWORD IOCPBase::WorkerThreadProc(LPVOID lpParam)
{
	IOCPBase* iocp = (IOCPBase*)lpParam;
	OVERLAPPED* ol = NULL;
	SocketContext* sockContext;
	DWORD dwBytes = 0;
	IOContext* ioContext = NULL;

	while (WAIT_OBJECT_0 != WaitForSingleObject(iocp->stopEvent, 0))
	{
		//DWORD类型修改为PULONG_PTR类型,兼容x64平台
		BOOL bRet = GetQueuedCompletionStatus(iocp->completionPort, &dwBytes, (PULONG_PTR)&sockContext, &ol, INFINITE);

		// 读取传入的参数
		ioContext = CONTAINING_RECORD(ol, IOContext, overLapped);

		// 收到退出标志
		if (EXIT_CODE == sockContext)
		{
			break;
		}
		if (!bRet)
		{
			DWORD dwErr = GetLastError();

			// 如果是超时了，就再继续等吧  
			if (WAIT_TIMEOUT == dwErr)
			{
				//// 确认客户端是否还活着...
				//if (!iocp->IsSocketAlive(sockContext->connSocket))
				//{
				//	iocp->OnConnectionClosed(sockContext);

				//	// 回收socket
				//	iocp->DoClose(sockContext);
				//	continue;
				//}
				//else
				//{
				//	continue;
				//}
				iocp->DoClose(sockContext);
			}
			// 可能是客户端异常退出了(64)
			else if (ERROR_NETNAME_DELETED == dwErr)
			{
				iocp->OnConnectionError(sockContext, dwErr);

				// 回收socket
				iocp->DoClose(sockContext);
				continue;
			}
			else
			{
				iocp->OnConnectionError(sockContext, dwErr);

				// 回收socket
				iocp->DoClose(sockContext);
				continue;
			}
		}
		else
		{
			// 判断是否有客户端断开
			if ((0 == dwBytes) && (RECV_POSTED == ioContext->ioType || SEND_POSTED == ioContext->ioType))
			{
				iocp->OnConnectionClosed(sockContext);

				// 回收socket
				iocp->DoClose(sockContext);
				continue;
			}
			else
			{
				switch (ioContext->ioType)
				{
				case ACCEPT_POSTED:
					iocp->DoAccpet(sockContext, ioContext);
					break;
				case RECV_POSTED:
					iocp->DoRecv(sockContext, ioContext);
					break;
				case SEND_POSTED:
					iocp->DoSend(sockContext, ioContext);
					break;
				default:
					break;
				}
			}
		}
	}

	// 释放线程参数
	//在线程释放进程的IOCPBase对象,造成内存异常
	//RELEASE(lpParam);
	return 0;
}

