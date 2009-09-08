/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2009  PCSX2 Dev Team
 * 
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "Common.h"
#include "System.h"
#include "SaveState.h"
#include "Elfheader.h"
#include "Plugins.h"
#include "CoreEmuThread.h"

#include "R5900.h"
#include "R3000A.h"
#include "VUmicro.h"

static __threadlocal CoreEmuThread* tls_coreThread = NULL;

CoreEmuThread& CoreEmuThread::Get()
{
	wxASSERT_MSG( tls_coreThread != NULL, L"This function must be called from the context of a running CoreEmuThread." );
	return *tls_coreThread;
}

void CoreEmuThread::CpuInitializeMess()
{
	GetPluginManager().Open();
	cpuReset();
	SysClearExecutionCache();
	GetPluginManager().Open();

	if( StateRecovery::HasState() )
	{
		// no need to boot bios or detect CDs when loading savestates.
		// [TODO] : It might be useful to detect game SLUS/CRC and compare it against
		// the savestate info, and issue a warning to the user since, chances are, they
		// don't really want to run a game with the wrong ISO loaded into the emu.
		StateRecovery::Recover();
	}
	else
	{
		wxString elf_file;
		if( EmuConfig.SkipBiosSplash )
		{
			// Fetch the ELF filename and CD type from the CDVD provider.
			wxString ename;
			int result = GetPS2ElfName( ename );
			switch( result )
			{
				case 0:
					throw Exception::RuntimeError( wxLt("Fast Boot failed: CDVD image is not a PS1 or PS2 game.") );

				case 1:
					throw Exception::RuntimeError( wxLt("Fast Boot failed: PCSX2 does not support emulation of PS1 games.") );

				case 2:
					// PS2 game.  Valid!
					elf_file = ename;
				break;

				jNO_DEFAULT
			}
		}

		if( !elf_file.IsEmpty() )
		{
			// Skip Bios Hack -- Runs the PS2 BIOS stub, and then manually loads the ELF
			// executable data, and injects the cpuRegs.pc with the address of the
			// execution start point.
			//
			// This hack is necessary for non-CD ELF files, and is optional for game CDs
			// (though not recommended for games because of rare ill side effects).

			cpuExecuteBios();
			loadElfFile( elf_file );
		}
	}
	
	if( GSsetGameCRC != NULL )
		GSsetGameCRC( ElfCRC, 0 );
}

// special macro which disables inlining on functions that require their own function stackframe.
// This is due to how Win32 handles structured exception handling.  Linux uses signals instead
// of SEH, and so these functions can be inlined.
#ifdef _WIN32
#	define __unique_stackframe __noinline
#else
#	define __unique_stackframe
#endif

// On Win32 this function invokes SEH, which requires it be in a function all by itself
// with inlining disabled.
__unique_stackframe
void CoreEmuThread::CpuExecute()
{
	PCSX2_MEM_PROTECT_BEGIN();
	Cpu->Execute();
	PCSX2_MEM_PROTECT_END();
}

static void _cet_callback_cleanup( void* handle )
{
	((CoreEmuThread*)handle)->DoThreadCleanup();
}

sptr CoreEmuThread::ExecuteTask()
{
	tls_coreThread = this;

	while( m_ExecMode != ExecMode_Running )
	{
		m_ResumeEvent.WaitGui();
	}

	CpuInitializeMess();
	StateCheck();
	CpuExecute();

	return 0;
}

void CoreEmuThread::StateCheck()
{
	switch( m_ExecMode )
	{
		case ExecMode_Idle:
			// threads should never have an idle execution state set while the
			// thread is in any way active or alive.
			DevAssert( false, "Invalid execution state detected." );
		break;

		// These are not the case statements you're looking for.  Move along.
		case ExecMode_Running:
			pthread_testcancel();
		break;

		case ExecMode_Suspending:
		{
			ScopedLock locker( m_lock_ExecMode );
			m_ExecMode = ExecMode_Suspended;
			m_SuspendEvent.Post();
		}

		case ExecMode_Suspended:
			while( m_ExecMode == ExecMode_Suspended )
				m_ResumeEvent.WaitGui();
		break;
	}
}

CoreEmuThread::CoreEmuThread() :
	m_ExecMode( ExecMode_Idle )
,	m_ResumeEvent()
,	m_SuspendEvent()
,	m_resetRecompilers( false )
,	m_resetProfilers( false )

,	m_lock_ExecMode()
{
	PersistentThread::Start();
}

// Invoked by the pthread_exit or pthread_cancel
void CoreEmuThread::DoThreadCleanup()
{
	GetPluginManager().Close();
	PersistentThread::DoThreadCleanup();
}

CoreEmuThread::~CoreEmuThread()
{
	PersistentThread::Cancel();
}

// Resumes the core execution state, or does nothing is the core is already running.  If
// settings were changed, resets will be performed as needed and emulation state resumed from
// memory savestates.
void CoreEmuThread::Resume()
{
	if( IsSelf() || !IsRunning() ) return;

	{
		ScopedLock locker( m_lock_ExecMode );

		if( m_ExecMode == ExecMode_Running )
			return;

		if( m_ExecMode == ExecMode_Suspending )
		{
			// if there are resets to be done, then we need to make sure and wait for the
			// emuThread to enter a fully suspended state before continuing...

			if( m_resetRecompilers || m_resetProfilers )
			{
				locker.Unlock();		// no deadlocks please, thanks. :)
				m_SuspendEvent.WaitGui();
			}
			else
			{
				m_ExecMode = ExecMode_Running;
				return;
			}
		}
	}

	DevAssert( (m_ExecMode == ExecMode_Suspended) || (m_ExecMode == ExecMode_Idle),
		"EmuCoreThread is not in a suspended or idle state?  wtf!" );

	if( m_resetRecompilers || m_resetProfilers )
	{
		SysClearExecutionCache();
		m_resetRecompilers = false;
		m_resetProfilers = false;
	}

	m_ExecMode = ExecMode_Running;
	m_ResumeEvent.Post();
}

// Pauses the emulation state at the next PS2 vsync, and returns control to the calling
// thread; or does nothing if the core is already suspended.  Calling this thread from the
// Core thread will result in deadlock.
//
// Parameters:
//   isNonblocking - if set to true then the function will not block for emulation suspension.
//      Defaults to false if parameter is not specified.  Performing non-blocking suspension
//      is mostly useful for starting certain non-Emu related gui activities (improves gui
//      responsiveness).
//
void CoreEmuThread::Suspend( bool isBlocking )
{
	if( IsSelf() || !IsRunning() ) return;

	{
		ScopedLock locker( m_lock_ExecMode );

		if( (m_ExecMode == ExecMode_Suspended) || (m_ExecMode == ExecMode_Idle) )
			return;

		if( m_ExecMode == ExecMode_Running )
			m_ExecMode = ExecMode_Suspending;

		DevAssert( m_ExecMode == ExecMode_Suspending, "ExecMode should be nothing other than Suspended..." );
	}

	m_SuspendEvent.WaitGui();
}

// Applies a full suite of new settings, which will automatically facilitate the necessary
// resets of the core and components (including plugins, if needed).  The scope of resetting
// is determined by comparing the current settings against the new settings.
void CoreEmuThread::ApplySettings( const Pcsx2Config& src )
{
	m_resetRecompilers = ( src.Cpu != EmuConfig.Cpu ) || ( src.Gamefixes != EmuConfig.Gamefixes ) || ( src.Speedhacks != EmuConfig.Speedhacks );
	m_resetProfilers = (src.Profiler != EmuConfig.Profiler );
	EmuConfig = src;
}

