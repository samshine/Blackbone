#include "Process.h"
#include "../Misc/NameResolve.h"
#include "../Misc/DynImport.h"

#include <memory>

namespace blackbone
{

Process::Process()
    : _core()
    , _modules( *this )
    , _memory( this )
    , _threads( _core )
    , _hooks( _memory )
    , _remote( *this )
    , _mmap( *this )
    , _nativeLdr( *this )
{
    GrantPriviledge( SE_DEBUG_NAME );
    GrantPriviledge( SE_LOAD_DRIVER_NAME );

    NameResolve::Instance().Initialize(); 
}

Process::~Process(void)
{
}

/// <summary>
/// Attach to existing process
/// </summary>
/// <param name="pid">Process ID</param>
/// <param name="access">Access mask</param>
/// <returns>Status code</returns>
NTSTATUS Process::Attach( DWORD pid, DWORD access /*= DEFAULT_ACCESS_P*/ )
{
    Detach();

    auto status = _core.Open( pid, access );
    if (NT_SUCCESS( status ))
    {
        _nativeLdr.Init();
        _remote.CreateRPCEnvironment( false, false );
    }

    return status;
}

/// <summary>
/// Attach to existing process
/// </summary>
/// <param name="pid">Process handle</param>
/// <returns>Status code</returns>
NTSTATUS Process::Attach( HANDLE hProc )
{
    Detach();

    auto status = _core.Open( hProc );
    if (NT_SUCCESS( status ))
    {
        _nativeLdr.Init();
        _remote.CreateRPCEnvironment( false, false );
    }

    return status;
}

/// <summary>
/// Create new process and attach to it
/// </summary>
/// <param name="path">Executable path</param>
/// <param name="suspended">Leave process in suspended state. To resume process one should resume its main thread</param>
/// <param name="forceInit">If 'suspended' is true, this flag will enforce process initialization via second thread</param>
/// <param name="cmdLine">Process command line</param>
/// <param name="currentDir">Startup directory</param>
/// <param name="pStartup">Additional startup params</param>
/// <returns>Status code</returns>
NTSTATUS Process::CreateAndAttach( 
    const std::wstring& path, 
    bool suspended /*= false*/,
    bool forceInit /*= true*/,
    const std::wstring& cmdLine /*= L""*/,
    const wchar_t* currentDir /*= nullptr*/,
    STARTUPINFOW* pStartup /*= nullptr*/
    )
{
    Detach();

    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    if (!pStartup)
        pStartup = &si;

    if (!CreateProcessW( path.c_str(), const_cast<LPWSTR>(cmdLine.c_str()), NULL, NULL, 
                         FALSE, CREATE_SUSPENDED, NULL, currentDir, pStartup, &pi ))
    {
        return LastNtStatus();
    } 

    // Get handle ownership
    auto status = _core.Open( pi.hProcess );
    if (NT_SUCCESS( status ))
    {
        _nativeLdr.Init();

        // Check if process must be left in suspended mode
        if (suspended)
        {
            // Create new thread to make sure LdrInitializeProcess gets called
            if (forceInit)
                EnsureInit();
        }
        else
            ResumeThread( pi.hThread );
    }

    // Close unneeded handles
    CloseHandle( pi.hThread );

    return status;
}

/// <summary>
/// Detach form current process, if any
/// </summary>
/// <returns>Status code</returns>
NTSTATUS Process::Detach()
{
    // Reset data
    _memory.reset();
    _modules.reset();
    _remote.reset();
    _mmap.reset();
    _threads.reset();
    _hooks.reset();
    _core.Close();

    return STATUS_SUCCESS;
}

/// <summary>
/// Ensure LdrInitializeProcess gets called
/// </summary>
/// <returns>Status code</returns>
NTSTATUS Process::EnsureInit()
{
    auto pProc = _modules.GetExport( _modules.GetModule( L"ntdll.dll", blackbone::Sections ), "NtYieldExecution" ).procAddress;
    if (pProc)
        return _remote.ExecDirect( pProc, 0 );

    return STATUS_NOT_FOUND;
}


/// <summary>
/// Checks if process still exists
/// </summary>
/// <returns></returns>
bool Process::valid()
{
    DWORD dwExitCode = 0;

    if (!_core.handle() || !GetExitCodeProcess( _core.handle(), &dwExitCode ))
        return false;

    return (dwExitCode == STILL_ACTIVE);
}

/// <summary>
/// Terminate process
/// </summary>
/// <param name="code">Exit code</param>
/// <returns>Stratus code</returns>
NTSTATUS Process::Terminate( uint32_t code /*= 0*/ )
{
    TerminateProcess( _core.handle(), code );
    return LastNtStatus();
}

/// <summary>
/// Grant current process arbitrary privilege
/// </summary>
/// <param name="name">Privilege name</param>
/// <returns>Status</returns>
NTSTATUS Process::GrantPriviledge( const std::basic_string<TCHAR>& name )
{
    TOKEN_PRIVILEGES Priv, PrivOld;
    DWORD cbPriv = sizeof(PrivOld);
    HANDLE hToken;

    if (!OpenThreadToken( GetCurrentThread(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, FALSE, &hToken ))
    {
        if (GetLastError() != ERROR_NO_TOKEN)
            return LastNtStatus();

        if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &hToken ))
            return LastNtStatus();
    }

    Priv.PrivilegeCount = 1;
    Priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValue( NULL, name.c_str(), &Priv.Privileges[0].Luid );

    if (!AdjustTokenPrivileges( hToken, FALSE, &Priv, sizeof(Priv), &PrivOld, &cbPriv ))
    {
        CloseHandle( hToken );
        return LastNtStatus();
    }

    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        CloseHandle( hToken );
        return LastNtStatus();
    }
    
    return STATUS_SUCCESS;
}

/// <summary>
/// Search for process by executable name
/// </summary>
/// <param name="name">Process name. If empty - function will retrieve all existing processes</param>
/// <param name="found">Found processses</param>
void Process::EnumByName( const std::wstring& name, std::vector<DWORD>& found )
{
    HANDLE hProcSnap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );

    if (hProcSnap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W tEntry = { 0 };
        tEntry.dwSize = sizeof(PROCESSENTRY32W);

        // Iterate threads
        for (BOOL success = Process32FirstW( hProcSnap, &tEntry );
              success == TRUE; 
              success = Process32NextW( hProcSnap, &tEntry ))
        {
            if (name.empty() || _wcsicmp( tEntry.szExeFile, name.c_str() ) == 0)
                found.emplace_back( tEntry.th32ProcessID );
        }

        CloseHandle( hProcSnap );
    }
}

/// <summary>
/// Search for process by executable name or by process ID
/// </summary>
/// <param name="pid">Target process ID. rocess name. If empty - function will retrieve all existing processes</param>
/// <param name="name">Process executable name. If empty - function will retrieve all existing processes</param>
/// <param name="found">Found processses</param>
/// <param name="includeThreads">If set to true, function will retrieve info ablout process threads</param>
/// <returns>Status code</returns>
NTSTATUS Process::EnumByNameOrPID(
    uint32_t pid,
    const std::wstring& name,
    std::vector<ProcessInfo>& found,
    bool includeThreads /*= false*/
    )
{
    ULONG bufSize = 0x100;
    uint8_t tmpbuf[0x100];
    uint8_t* buffer = tmpbuf;
    ULONG returnLength = 0;

    found.clear();

    // Query process info
    NTSTATUS status = GET_IMPORT( NtQuerySystemInformation )((SYSTEM_INFORMATION_CLASS)57, buffer, bufSize, &returnLength);
    if (!NT_SUCCESS( status ))
    {
        bufSize = returnLength;
        buffer = (uint8_t*)VirtualAlloc( NULL, bufSize, MEM_COMMIT, PAGE_READWRITE );
        status = GET_IMPORT( NtQuerySystemInformation )((SYSTEM_INFORMATION_CLASS)57, buffer, bufSize, &returnLength);
        if (!NT_SUCCESS( status ))
        {
            VirtualFree( buffer, 0, MEM_RELEASE );
            return status;
        }
    }

    // Parse info
    for (auto pInfo = reinterpret_cast<_SYSTEM_PROCESS_INFORMATION_T<DWORD_PTR>*>(buffer);;)
    {
        //  Skip idle process, compare name or compare pid
        if (pInfo->UniqueProcessId != 0 && (
            name.empty() && pid == 0 ||
            _wcsicmp( name.c_str(), (wchar_t*)pInfo->ImageName.Buffer ) == 0 ||
            pid == pInfo->UniqueProcessId))
        {
            ProcessInfo info;

            info.pid = static_cast<uint32_t>(pInfo->UniqueProcessId);

            if (pInfo->ImageName.Buffer)
                info.imageName = reinterpret_cast<wchar_t*>(pInfo->ImageName.Buffer);

            // Get threads info
            if (includeThreads)
            {
                int64_t minTime = 0xFFFFFFFFFFFFFFFF;
                ULONG mainIdx = 0;

                for (ULONG i = 0; i < pInfo->NumberOfThreads; i++)
                {
                    ThreadInfo tinfo;
                    auto& thd = pInfo->Threads[i];

                    tinfo.tid = static_cast<uint32_t>(thd.ThreadInfo.ClientId.UniqueThread);
                    tinfo.startAddress = static_cast<uintptr_t>(thd.ThreadInfo.StartAddress);

                    // Check for main thread
                    if (thd.ThreadInfo.CreateTime.QuadPart < minTime)
                    {
                        minTime = thd.ThreadInfo.CreateTime.QuadPart;
                        mainIdx = i;
                    }

                    info.threads.emplace_back( tinfo );
                    info.threads[mainIdx].mainThread = true;
                }
            }

            found.emplace_back( info );
        }

        if (pInfo->NextEntryOffset)
            pInfo = reinterpret_cast<_SYSTEM_PROCESS_INFORMATION_T<DWORD_PTR>*>((uint8_t*)pInfo + pInfo->NextEntryOffset);
        else
            break;        
    }

    // Sort results
    std::sort( found.begin(), found.end() );

    VirtualFree( buffer, 0, MEM_RELEASE );
    return status;
}


}