/*
copyright (C) 2011 by high828@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
 */

#include "stdafx.h"
#include <Windows.h>
#include <string>
#include <algorithm>
#include "Detours\Detours.h"
#include <vector>
#include <iostream>
#include <boost/interprocess/sync/sharable_lock.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/thread.hpp>
#include "BufferQueue.h"
#include <WinSock2.h>

bool hasEnding (std::string const &fullString, std::string const &ending)
{
    if (fullString.length() >= ending.length()) {
        return (0 == fullString.compare (fullString.length() - ending.length(), ending.length(), ending));
    } else {
        return false;
    }
}
void lower(std::string &data)
{
	std::transform(data.begin(), data.end(),
	data.begin(), ::tolower);
}

bool islol()
{
	CHAR buffer[1000];
	DWORD length = GetModuleFileNameA(NULL, buffer, 1000);
	std::string str(buffer, length);
	lower(str);

	return hasEnding(str, "lolclient.exe");
}

std::wstring StringToWString(const std::string& s)
{
	std::wstring temp(s.length(),L' ');
	std::copy(s.begin(), s.end(), temp.begin());
	return temp;
}


std::string WStringToString(const std::wstring& s)
{
	std::string temp(s.length(), ' ');
	std::copy(s.begin(), s.end(), temp.begin());
	return temp;
}


bool IsLoL = false;
HANDLE LogHandle = 0;
HANDLE SelfLogHandle = 0;
boost::condition cond;
boost::mutex mutex;
const int buffermax = 0x500000;
const int datamax = 0xA00000;
const LPTSTR pipename = TEXT("\\\\.\\pipe\\lolnotes");
DWORD timeout = 0;
BufferQueue Buffers(buffermax);

static HANDLE (WINAPI * TrueCreateFileW)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) = CreateFileW;
static BOOL (WINAPI * TrueWriteFile)(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped) = WriteFile;
static BOOL (WINAPI * Trueconnect)(SOCKET s, const struct sockaddr FAR * name, int namelen) = connect;

std::string format(const char *fmt, ...) 
{ 
   using std::string;
   using std::vector;

   string retStr("");

   if (NULL != fmt)
   {
      va_list marker = NULL; 

      // initialize variable arguments
      va_start(marker, fmt); 
      
      // Get formatted string length adding one for NULL
      size_t len = _vscprintf(fmt, marker) + 1;
               
      // Create a char vector to hold the formatted string.
      vector<char> buffer(len, '\0');
      int nWritten = _vsnprintf_s(&buffer[0], buffer.size(), len, fmt, marker);    

      if (nWritten > 0)
      {
         retStr = &buffer[0];
      }
            
      // Reset variable arguments
      va_end(marker); 
   }

   return retStr; 
}
static BOOL WriteString(HANDLE file, char* fmt, ...)
{
	if (file == INVALID_HANDLE_VALUE)
		return false;
	using std::string;
	using std::vector;

	string retStr("");

	if (NULL != fmt)
	{
		va_list marker = NULL; 

		// initialize variable arguments
		va_start(marker, fmt); 
      
		// Get formatted string length adding one for NULL
		size_t len = _vscprintf(fmt, marker) + 1;
               
		// Create a char vector to hold the formatted string.
		vector<char> buffer(len, '\0');
		int nWritten = _vsnprintf_s(&buffer[0], buffer.size(), len, fmt, marker);    

		if (nWritten > 0)
		{
			retStr = &buffer[0];
		}
            
		// Reset variable arguments
		va_end(marker); 
	}

	DWORD written;
	return WriteFile(file, (LPCVOID)retStr.c_str(), retStr.length(), &written, NULL);
}
static std::string GetDirectory(const std::string &data)
{
	std::string str = data;
	for(int i = 0; i < str.length(); i++)
	{
		if (str[i] == '/')
			str[i] = '\\';
	}

	int find = str.find_last_of('\\');
	if (find != -1)
		str = str.substr(0, find);

	return str;
}

static std::string GetModuleName(HMODULE module)
{
	char buffer[1000];
	DWORD len = GetModuleFileNameA(module, buffer, 1000);
	return std::string(buffer, len);
}

DWORD WINAPI InternalClientLoop(LPVOID ptr)
{
	while (true)
	{
		WriteString(SelfLogHandle, "Creating pipe\n");
		HANDLE server = CreateNamedPipe( 
			pipename,				  // pipe name 
			PIPE_ACCESS_DUPLEX,       // read access 
			PIPE_TYPE_BYTE |       // message type pipe 
			PIPE_READMODE_BYTE |   // message-read mode 
			PIPE_WAIT,                // blocking mode 
			PIPE_UNLIMITED_INSTANCES, // max. instances  
			datamax,                  // output buffer size 
			datamax,                  // input buffer size 
			0,                        // client time-out 
			NULL);                    // default security attribute 
		if (server == INVALID_HANDLE_VALUE)
		{
			WriteString(SelfLogHandle, "Failed to create pipe (%ld)\n", GetLastError());
			return -1;
		}

		WriteString(SelfLogHandle, "Accepting on %ld\n", server);

		if (!(ConnectNamedPipe(server, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED)))
		{
			WriteString(SelfLogHandle, "Failed to accept client (%ld)\n", GetLastError());
			return -1;
		}

		WriteString(SelfLogHandle, "Accepted\n");

		while (true)
		{
			{
				boost::mutex::scoped_lock lk(mutex);
				while (Buffers.Size() < 1)
					cond.wait(lk);
			}

			{
				boost::mutex::scoped_lock lk(mutex);

				Buffer buf = Buffers.PopBuffer();
				if (buf.Size > 0)
				{
					DWORD written;
					if (!WriteFile(server, buf.Data.get(), buf.Size, &written, NULL))
					{
						WriteString(SelfLogHandle, "Failed to send to client (%ld)\n", GetLastError());
						break;
					}
					WriteString(SelfLogHandle, "Sent %d bytes\n", buf.Size);
					FlushFileBuffers(server);
				}
			}
		}

		CloseHandle(server);
	}
	return 0;
}

DWORD WINAPI ClientLoop(LPVOID ptr)
{
	WriteString(SelfLogHandle, "Pipe loop created\n");
	DWORD ret = InternalClientLoop(ptr);
	WriteString(SelfLogHandle, "Pipe loop ended\n");
	return ret;
}

HANDLE WINAPI MyCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
	HANDLE ret = TrueCreateFileW(lpFileName, dwDesiredAccess, dwShareMode | FILE_SHARE_READ, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

	if (!LogHandle)
	{
		std::wstring wfile(lpFileName);
		std::string file = WStringToString(wfile);

		lower(file);
		if (hasEnding(file, ".log") && file.find("lolclient") != -1)
		{
			if (!SelfLogHandle)
			{
				std::string dir = GetDirectory(GetModuleName(NULL));
				std::string file = dir + "\\lolnotes.log";
				SelfLogHandle = CreateFileA(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, NULL, NULL);
			}
			if (!CreateThread(NULL, NULL, ClientLoop, NULL, NULL, NULL))
			{
				WriteString(SelfLogHandle, "Failed to create server thread (%ld)\n", GetLastError());
			}
			else
			{
				WriteString(SelfLogHandle, "Started\n");
				LogHandle = ret;
			}
		}
	}
    return ret;
}

BOOL WINAPI MyWriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
	if (LogHandle && hFile == LogHandle)
	{
		{
			boost::mutex::scoped_lock lk(mutex);
			if (nNumberOfBytesToWrite > buffermax)
			{
				WriteString(SelfLogHandle, "Buffer exceeds %d\n", buffermax);
			}
			else
			{
				Buffers.ForcePushBuffer((char*)lpBuffer, nNumberOfBytesToWrite);
				cond.notify_all();
			}
		}
	}

    return TrueWriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

DWORD WINAPI UnloadSelf(LPVOID ptr)
{
	FreeLibraryAndExitThread((HMODULE)ptr, 0);
	return 0;
}

int __stdcall Myconnect(SOCKET s, const struct sockaddr FAR * name, int namelen)
{
	sockaddr_in * in = (sockaddr_in*)name;

	char* ip = inet_ntoa(in->sin_addr);

	WriteString(SelfLogHandle, "Connecting to %s:%d (connect)\n", ip, _byteswap_ushort(in->sin_port));

	if (in->sin_family == AF_INET && _byteswap_ushort(in->sin_port) == 2099)
	{
		in->sin_addr.s_addr = inet_addr("127.0.0.1");
	}

	return Trueconnect(s, name, namelen);
}

BOOL APIENTRY DllMain( HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	LONG error;
	switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
		{
			IsLoL = islol();

			if (IsLoL)
			{
				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourAttach(&(PVOID&)TrueCreateFileW, MyCreateFileW);
				error = DetourTransactionCommit();

				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourAttach(&(PVOID&)TrueWriteFile, MyWriteFile);
				error = DetourTransactionCommit();

				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourAttach(&(PVOID&)Trueconnect, Myconnect);
				error = DetourTransactionCommit();
			}
			else
			{
				CreateThread(NULL, NULL, UnloadSelf, hModule, NULL, NULL);
			}

			break;
		}
		case DLL_PROCESS_DETACH:
		{
			if (IsLoL)
			{
				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourDetach(&(PVOID&)TrueCreateFileW, MyCreateFileW);
				error = DetourTransactionCommit();

				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourDetach(&(PVOID&)TrueWriteFile, MyWriteFile);
				error = DetourTransactionCommit();

				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourDetach(&(PVOID&)Trueconnect, Myconnect);
				error = DetourTransactionCommit();
			}

			break;
		}
	}
	return TRUE;
}

