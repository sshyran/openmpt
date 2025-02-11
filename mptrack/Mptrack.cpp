/*
 * MPTrack.cpp
 * -----------
 * Purpose: OpenMPT core application class.
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#include "stdafx.h"
#include "Mptrack.h"
#include "Mainfrm.h"
#include "IPCWindow.h"
#include "InputHandler.h"
#include "Childfrm.h"
#include "Moddoc.h"
#include "ModDocTemplate.h"
#include "Globals.h"
#include "../soundlib/Dlsbank.h"
#include "../common/version.h"
#include "../test/test.h"
#include "UpdateCheck.h"
#include "../common/mptStringBuffer.h"
#include "ExceptionHandler.h"
#include "CloseMainDialog.h"
#include "PlugNotFoundDlg.h"
#include "AboutDialog.h"
#include "AutoSaver.h"
#include "FileDialog.h"
#include "Image.h"
#include "BuildVariants.h"
#include "../common/ComponentManager.h"
#include "WelcomeDialog.h"
#include "../sounddev/SoundDeviceManager.h"
#include "../soundlib/plugins/PluginManager.h"
#include "MPTrackWine.h"
#include "MPTrackUtil.h"

// GDI+
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define min(a,b) (((a) < (b)) ? (a) : (b))
#pragma warning(push)
#pragma warning(disable:4458) // declaration of 'x' hides class member
#include <gdiplus.h>
#pragma warning(pop)

// rewbs.memLeak
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
//end  rewbs.memLeak

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif


OPENMPT_NAMESPACE_BEGIN

/////////////////////////////////////////////////////////////////////////////
// The one and only CTrackApp object

CTrackApp theApp;

const TCHAR *szSpecialNoteNamesMPT[] = {_T("PCs"), _T("PC"), _T("~~ (Note Fade)"), _T("^^ (Note Cut)"), _T("== (Note Off)")};
const TCHAR *szSpecialNoteShortDesc[] = {_T("Param Control (Smooth)"), _T("Param Control"), _T("Note Fade"), _T("Note Cut"), _T("Note Off")};

// Make sure that special note arrays include string for every note.
static_assert(NOTE_MAX_SPECIAL - NOTE_MIN_SPECIAL + 1 == mpt::array_size<decltype(szSpecialNoteNamesMPT)>::size);
static_assert(mpt::array_size<decltype(szSpecialNoteShortDesc)>::size == mpt::array_size<decltype(szSpecialNoteNamesMPT)>::size);

const char *szHexChar = "0123456789ABCDEF";

void CTrackApp::OnFileCloseAll()
{
	if(!(TrackerSettings::Instance().m_dwPatternSetup & PATTERN_NOCLOSEDIALOG))
	{
		// Show modified documents window
		CloseMainDialog dlg;
		if(dlg.DoModal() != IDOK)
		{
			return;
		}
	}

	for(auto &doc : GetOpenDocuments())
	{
		doc->SafeFileClose();
	}
}


void CTrackApp::OnUpdateAnyDocsOpen(CCmdUI *cmd)
{
	cmd->Enable(!GetModDocTemplate()->empty());
}


int CTrackApp::GetOpenDocumentCount() const
{
	return static_cast<int>(GetModDocTemplate()->size());
}


// Retrieve a list of all open modules.
std::vector<CModDoc *> CTrackApp::GetOpenDocuments() const
{
	std::vector<CModDoc *> documents;

	CDocTemplate *pDocTmpl = GetModDocTemplate();
	if(pDocTmpl)
	{
		POSITION pos = pDocTmpl->GetFirstDocPosition();
		CDocument *pDoc;
		while((pos != nullptr) && ((pDoc = pDocTmpl->GetNextDoc(pos)) != nullptr))
		{
			documents.push_back(dynamic_cast<CModDoc *>(pDoc));
		}
	}

	return documents;
}


/////////////////////////////////////////////////////////////////////////////
// Command Line options

class CMPTCommandLineInfo: public CCommandLineInfo
{
public:
	std::vector<mpt::PathString> m_fileNames;
	bool m_noDls = false, m_noPlugins = false, m_noAssembly = false, m_noSysCheck = false, m_noWine = false,
		m_portable = false, m_noCrashHandler = false, m_debugCrashHandler = false, m_sharedInstance = false;
#ifdef ENABLE_TESTS
	bool m_noTests = false;
#endif

public:
	void ParseParam(LPCTSTR param, BOOL isFlag, BOOL isLast) override
	{
		if(isFlag)
		{
			if(!lstrcmpi(param, _T("nologo"))) { m_bShowSplash = FALSE; return; }
			if(!lstrcmpi(param, _T("nodls"))) { m_noDls = true; return; }
			if(!lstrcmpi(param, _T("noplugs"))) { m_noPlugins = true; return; }
			if(!lstrcmpi(param, _T("portable"))) { m_portable = true; return; }
			if(!lstrcmpi(param, _T("fullMemDump"))) { ExceptionHandler::fullMemDump = true; return; }
			if(!lstrcmpi(param, _T("noAssembly"))) { m_noAssembly = true; return; }
			if(!lstrcmpi(param, _T("noSysCheck"))) { m_noSysCheck = true; return; }
			if(!lstrcmpi(param, _T("noWine"))) { m_noWine = true; return; }
			if(!lstrcmpi(param, _T("noCrashHandler"))) { m_noCrashHandler = true; return; }
			if(!lstrcmpi(param, _T("DebugCrashHandler"))) { m_debugCrashHandler = true; return; }
			if(!lstrcmpi(param, _T("shared"))) { m_sharedInstance = true; return; }
#ifdef ENABLE_TESTS
			if (!lstrcmpi(param, _T("noTests"))) { m_noTests = true; return; }
#endif
		} else
		{
			m_fileNames.push_back(mpt::PathString::FromNative(param));
			if(m_nShellCommand == FileNew) m_nShellCommand = FileOpen;
		}
		CCommandLineInfo::ParseParam(param, isFlag, isLast);
	}
};


// Splash Screen

static void StartSplashScreen();
static void StopSplashScreen();
static void TimeoutSplashScreen();


/////////////////////////////////////////////////////////////////////////////
// Midi Library

MidiLibrary CTrackApp::midiLibrary;

void CTrackApp::ImportMidiConfig(const mpt::PathString &filename, bool hideWarning)
{
	if(filename.empty()) return;

	if(CDLSBank::IsDLSBank(filename))
	{
		ConfirmAnswer result = cnfYes;
		if(!hideWarning)
		{
			result = Reporting::Confirm("You are about to replace the current MIDI library:\n"
				"Do you want to replace only the missing instruments? (recommended)",
				"Warning", true);
		}
		if(result == cnfCancel) return;
		const bool replaceAll = (result == cnfNo);
		CDLSBank dlsbank;
		if (dlsbank.Open(filename))
		{
			for(uint32 ins = 0; ins < 256; ins++)
			{
				if(replaceAll || midiLibrary[ins].empty())
				{
					uint32 prog = (ins < 128) ? ins : 0xFF;
					uint32 key = (ins < 128) ? 0xFF : ins & 0x7F;
					uint32 bank = (ins < 128) ? 0 : F_INSTRUMENT_DRUMS;
					if (dlsbank.FindInstrument(ins >= 128, bank, prog, key))
					{
						midiLibrary[ins] = filename;
					}
				}
			}
		}
		return;
	}

	IniFileSettingsContainer file(filename);
	ImportMidiConfig(file, filename.GetPath());
}


static mpt::PathString GetUltraSoundPatchDir(SettingsContainer &file, const mpt::ustring &iniSection, const mpt::PathString &path, bool forgetSettings)
{
	mpt::PathString patchDir = file.Read<mpt::PathString>(iniSection, U_("PatchDir"), {});
	if(forgetSettings)
		file.Forget(U_("Ultrasound"), U_("PatchDir"));
	if(patchDir.empty() || patchDir == P_(".\\"))
		patchDir = path;
	if(!patchDir.empty())
		patchDir.EnsureTrailingSlash();
	return patchDir;
}

void CTrackApp::ImportMidiConfig(SettingsContainer &file, const mpt::PathString &path, bool forgetSettings)
{
	const mpt::PathString patchDir = GetUltraSoundPatchDir(file, U_("Ultrasound"), path, forgetSettings);
	for(uint32 prog = 0; prog < 256; prog++)
	{
		mpt::ustring key = MPT_UFORMAT("{}{}")((prog < 128) ? U_("Midi") : U_("Perc"), prog & 0x7F);
		mpt::PathString filename = file.Read<mpt::PathString>(U_("Midi Library"), key, mpt::PathString());
		// Check for ULTRASND.INI
		if(filename.empty())
		{
			mpt::ustring section = (prog < 128) ? UL_("Melodic Patches") : UL_("Drum Patches");
			key = mpt::ufmt::val(prog & 0x7f);
			filename = file.Read<mpt::PathString>(section, key, mpt::PathString());
			if(forgetSettings) file.Forget(section, key);
			if(filename.empty())
			{
				section = (prog < 128) ? UL_("Melodic Bank 0") : UL_("Drum Bank 0");
				filename = file.Read<mpt::PathString>(section, key, mpt::PathString());
				if(forgetSettings) file.Forget(section, key);
			}
			const mpt::PathString localPatchDir = GetUltraSoundPatchDir(file, section, patchDir, forgetSettings);
			if(!filename.empty())
			{
				filename = localPatchDir + filename + P_(".pat");
			}
		}
		if(!filename.empty())
		{
			filename = theApp.PathInstallRelativeToAbsolute(filename);
			midiLibrary[prog] = filename;
		}
	}
}


void CTrackApp::ExportMidiConfig(const mpt::PathString &filename)
{
	if(filename.empty()) return;
	IniFileSettingsContainer file(filename);
	ExportMidiConfig(file);
}

void CTrackApp::ExportMidiConfig(SettingsContainer &file)
{
	for(uint32 prog = 0; prog < 256; prog++) if (!midiLibrary[prog].empty())
	{
		mpt::PathString szFileName = midiLibrary[prog];

		if(!szFileName.empty())
		{
			if(theApp.IsPortableMode())
				szFileName = theApp.PathAbsoluteToInstallRelative(szFileName);

			mpt::ustring key = MPT_UFORMAT("{}{}")((prog < 128) ? U_("Midi") : U_("Perc"), prog & 0x7F);
			file.Write<mpt::PathString>(U_("Midi Library"), key, szFileName);
		}
	}
}


/////////////////////////////////////////////////////////////////////////////
// DLS Banks support

std::vector<CDLSBank *> CTrackApp::gpDLSBanks;


void CTrackApp::LoadDefaultDLSBanks()
{
	uint32 numBanks = theApp.GetSettings().Read<uint32>(U_("DLS Banks"), U_("NumBanks"), 0);
	gpDLSBanks.reserve(numBanks);
	for(uint32 i = 0; i < numBanks; i++)
	{
		mpt::PathString path = theApp.GetSettings().Read<mpt::PathString>(U_("DLS Banks"), MPT_UFORMAT("Bank{}")(i + 1), mpt::PathString());
		path = theApp.PathInstallRelativeToAbsolute(path);
		AddDLSBank(path);
	}

	HKEY key;
	if(RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("Software\\Microsoft\\DirectMusic"), 0, KEY_READ, &key) == ERROR_SUCCESS)
	{
		DWORD dwRegType = REG_SZ;
		DWORD dwSize = 0;
		if(RegQueryValueEx(key, _T("GMFilePath"), NULL, &dwRegType, nullptr, &dwSize) == ERROR_SUCCESS && dwSize > 0)
		{
			std::vector<TCHAR> filenameT(dwSize / sizeof(TCHAR));
			if (RegQueryValueEx(key, _T("GMFilePath"), NULL, &dwRegType, reinterpret_cast<LPBYTE>(filenameT.data()), &dwSize) == ERROR_SUCCESS)
			{
				std::vector<TCHAR> filenameExpanded(::ExpandEnvironmentStrings(filenameT.data(), nullptr, 0));
				::ExpandEnvironmentStrings(filenameT.data(), filenameExpanded.data(), static_cast<DWORD>(filenameExpanded.size()));
				auto filename = mpt::PathString::FromNative(filenameExpanded.data());
				AddDLSBank(filename);
				ImportMidiConfig(filename, true);
			}
		}
		RegCloseKey(key);
	}
}


void CTrackApp::SaveDefaultDLSBanks()
{
	uint32 nBanks = 0;
	for(const auto &bank : gpDLSBanks)
	{
		if(!bank || bank->GetFileName().empty())
			continue;

		mpt::PathString path = bank->GetFileName();
		if(theApp.IsPortableMode())
		{
			path = theApp.PathAbsoluteToInstallRelative(path);
		}

		mpt::ustring key = MPT_UFORMAT("Bank{}")(nBanks + 1);
		theApp.GetSettings().Write<mpt::PathString>(U_("DLS Banks"), key, path);
		nBanks++;

	}
	theApp.GetSettings().Write<uint32>(U_("DLS Banks"), U_("NumBanks"), nBanks);
}


void CTrackApp::RemoveDLSBank(UINT nBank)
{
	if(nBank >= gpDLSBanks.size() || !gpDLSBanks[nBank]) return;
	delete gpDLSBanks[nBank];
	gpDLSBanks[nBank] = nullptr;
	//gpDLSBanks.erase(gpDLSBanks.begin() + nBank);
}


bool CTrackApp::AddDLSBank(const mpt::PathString &filename)
{
	if(filename.empty() || !CDLSBank::IsDLSBank(filename)) return false;
	// Check for dupes
	for(const auto &bank : gpDLSBanks)
	{
		if(bank && !mpt::PathString::CompareNoCase(filename, bank->GetFileName())) return true;
	}
	CDLSBank *bank = nullptr;
	try
	{
		bank = new CDLSBank;
		if(bank->Open(filename))
		{
			gpDLSBanks.push_back(bank);
			return true;
		}
	} catch(mpt::out_of_memory e)
	{
		mpt::delete_out_of_memory(e);
	} catch(const std::exception &)
	{
	}
	delete bank;
	return false;
}


/////////////////////////////////////////////////////////////////////////////
// CTrackApp

MODTYPE CTrackApp::m_nDefaultDocType = MOD_TYPE_IT;

BEGIN_MESSAGE_MAP(CTrackApp, CWinApp)
	//{{AFX_MSG_MAP(CTrackApp)
	ON_COMMAND(ID_FILE_NEW,		&CTrackApp::OnFileNew)
	ON_COMMAND(ID_FILE_NEWMOD,	&CTrackApp::OnFileNewMOD)
	ON_COMMAND(ID_FILE_NEWS3M,	&CTrackApp::OnFileNewS3M)
	ON_COMMAND(ID_FILE_NEWXM,	&CTrackApp::OnFileNewXM)
	ON_COMMAND(ID_FILE_NEWIT,	&CTrackApp::OnFileNewIT)
	ON_COMMAND(ID_NEW_MPT,		&CTrackApp::OnFileNewMPT)
	ON_COMMAND(ID_FILE_OPEN,	&CTrackApp::OnFileOpen)
	ON_COMMAND(ID_FILE_CLOSEALL, &CTrackApp::OnFileCloseAll)
	ON_COMMAND(ID_APP_ABOUT,	&CTrackApp::OnAppAbout)
	ON_UPDATE_COMMAND_UI(ID_FILE_CLOSEALL, &CTrackApp::OnUpdateAnyDocsOpen)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CTrackApp construction

CTrackApp::CTrackApp()
{
	m_dwRestartManagerSupportFlags = AFX_RESTART_MANAGER_SUPPORT_RESTART | AFX_RESTART_MANAGER_REOPEN_PREVIOUS_FILES;
}


void CTrackApp::AddToRecentFileList(LPCTSTR lpszPathName)
{
	AddToRecentFileList(mpt::PathString::FromCString(lpszPathName));
}


void CTrackApp::AddToRecentFileList(const mpt::PathString &path)
{
	RemoveMruItem(path);
	TrackerSettings::Instance().mruFiles.insert(TrackerSettings::Instance().mruFiles.begin(), path);
	if(TrackerSettings::Instance().mruFiles.size() > TrackerSettings::Instance().mruListLength)
	{
		TrackerSettings::Instance().mruFiles.resize(TrackerSettings::Instance().mruListLength);
	}
	CMainFrame::GetMainFrame()->UpdateMRUList();
}


void CTrackApp::RemoveMruItem(const size_t item)
{
	if(item < TrackerSettings::Instance().mruFiles.size())
	{
		TrackerSettings::Instance().mruFiles.erase(TrackerSettings::Instance().mruFiles.begin() + item);
		CMainFrame::GetMainFrame()->UpdateMRUList();
	}
}


void CTrackApp::RemoveMruItem(const mpt::PathString &path)
{
	auto &mruFiles = TrackerSettings::Instance().mruFiles;
	for(auto i = mruFiles.begin(); i != mruFiles.end(); i++)
	{
		if(!mpt::PathString::CompareNoCase(*i, path))
		{
			mruFiles.erase(i);
			break;
		}
	}
}


/////////////////////////////////////////////////////////////////////////////
// CTrackApp initialization


namespace Tracker
{
mpt::recursive_mutex_with_lock_count & GetGlobalMutexRef()
{
	return theApp.GetGlobalMutexRef();
}
} // namespace Tracker


class ComponentManagerSettings
	: public IComponentManagerSettings
{
private:
	TrackerSettings &conf;
	mpt::PathString configPath;
public:
	ComponentManagerSettings(TrackerSettings &conf, const mpt::PathString &configPath)
		: conf(conf)
		, configPath(configPath)
	{
		return;
	}
	bool LoadOnStartup() const override
	{
		return conf.ComponentsLoadOnStartup;
	}
	bool KeepLoaded() const override
	{
		return conf.ComponentsKeepLoaded;
	}
	bool IsBlocked(const std::string &key) const override
	{
		return conf.IsComponentBlocked(key);
	}
	mpt::PathString Path() const override
	{
		if(mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture()).empty())
		{
			return mpt::PathString();
		}
		return configPath + P_("Components\\") + mpt::PathString::FromUnicode(mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture())) + P_("\\");
	}
};


// Move a config file called fileName from the App's directory (or one of its sub directories specified by subDir) to
// %APPDATA%. If specified, it will be renamed to newFileName. Existing files are never overwritten.
// Returns true on success.
bool CTrackApp::MoveConfigFile(const mpt::PathString &fileName, mpt::PathString subDir, mpt::PathString newFileName)
{
	const mpt::PathString oldPath = GetInstallPath() + subDir + fileName;
	mpt::PathString newPath = GetConfigPath() + subDir;
	if(!newFileName.empty())
		newPath += newFileName;
	else
		newPath += fileName;

	if(!newPath.IsFile() && oldPath.IsFile())
	{
		return MoveFile(oldPath.AsNative().c_str(), newPath.AsNative().c_str()) != 0;
	}
	return false;
}


// Set up paths were configuration data is written to. Set overridePortable to true if application's own directory should always be used.
void CTrackApp::SetupPaths(bool overridePortable)
{

	// First, determine if the executable is installed in multi-arch mode or in the old standard mode.
	bool modeMultiArch = false;
	bool modeSourceProject = false;
	const mpt::PathString exePath = mpt::GetExecutablePath();
	auto exePathComponents = mpt::String::Split<mpt::ustring>(exePath.GetDir().WithoutTrailingSlash().ToUnicode(), P_("\\").ToUnicode());
	if(exePathComponents.size() >= 2)
	{
		if(exePathComponents[exePathComponents.size()-1] == mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture()))
		{
			if(exePathComponents[exePathComponents.size()-2] == U_("bin"))
			{
				modeMultiArch = true;
			}
		}
	}
	// Check if we are running from the source tree.
	if(!modeMultiArch && exePathComponents.size() >= 4)
	{
		if(exePathComponents[exePathComponents.size()-1] == mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture()))
		{
			if(exePathComponents[exePathComponents.size()-4] == U_("bin"))
			{
				modeSourceProject = true;
			}
		}
	}
	if(modeSourceProject)
	{
		m_InstallPath = mpt::GetAbsolutePath(exePath + P_("..\\") + P_("..\\") + P_("..\\") + P_("..\\"));
		m_InstallBinPath = mpt::GetAbsolutePath(exePath + P_("..\\"));
		m_InstallBinArchPath = exePath;
		m_InstallPkgPath = mpt::GetAbsolutePath(exePath + P_("..\\") + P_("..\\") + P_("..\\") + P_("..\\packageTemplate\\"));
	} else if(modeMultiArch)
	{
		m_InstallPath = mpt::GetAbsolutePath(exePath + P_("..\\") + P_("..\\"));
		m_InstallBinPath = mpt::GetAbsolutePath(exePath + P_("..\\"));
		m_InstallBinArchPath = exePath;
		m_InstallPkgPath = mpt::GetAbsolutePath(exePath + P_("..\\") + P_("..\\"));
	} else
	{
		m_InstallPath = exePath;
		m_InstallBinPath = exePath;
		m_InstallBinArchPath = exePath;
		m_InstallPkgPath = exePath;
	}

	// Determine paths, portable mode, first run. Do not yet update any state.
	mpt::PathString configPathPortable = (modeSourceProject ? exePath : m_InstallPath); // config path in portable mode
	mpt::PathString configPathUser; // config path in default non-portable mode
	{
		// Try to find a nice directory where we should store our settings (default: %APPDATA%)
		TCHAR dir[MAX_PATH] = { 0 };
		if((SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, dir) == S_OK)
			|| (SHGetFolderPath(NULL, CSIDL_MYDOCUMENTS, NULL, SHGFP_TYPE_CURRENT, dir) == S_OK))
		{
			// Store our app settings in %APPDATA% or "My Documents"
			configPathUser = mpt::PathString::FromNative(dir) + P_("\\OpenMPT\\");
		}
	}

	// Check if the user has configured portable mode.
	bool configInstallPortable = false;
	mpt::PathString portableFlagFilename = (configPathPortable + P_("OpenMPT.portable"));
	bool configPortableFlag = portableFlagFilename.IsFile();
	configInstallPortable = configInstallPortable || configPortableFlag;
	// before 1.29.00.13:
	configInstallPortable = configInstallPortable || (GetPrivateProfileInt(_T("Paths"), _T("UseAppDataDirectory"), 1, (configPathPortable + P_("mptrack.ini")).AsNative().c_str()) == 0);
	// convert to new style
	if(configInstallPortable && !configPortableFlag)
	{
		mpt::SafeOutputFile f(portableFlagFilename);
	}

	// Determine portable mode.
	bool portableMode = overridePortable || configInstallPortable || configPathUser.empty();

	// Update config dir
	m_ConfigPath = portableMode ? configPathPortable : configPathUser;

	// Set up default file locations
	m_szConfigFileName = m_ConfigPath + P_("mptrack.ini"); // config file
	m_szPluginCacheFileName = m_ConfigPath + P_("plugin.cache"); // plugin cache

	// Force use of custom ini file rather than windowsDir\executableName.ini
	if(m_pszProfileName)
	{
		free((void *)m_pszProfileName);
	}
	m_pszProfileName = _tcsdup(m_szConfigFileName.ToCString());

	m_bInstallerMode = !modeSourceProject && !portableMode;
	m_bPortableMode = portableMode;
	m_bSourceTreeMode = modeSourceProject;

}


void CTrackApp::CreatePaths()
{
	// Create missing diretories
	if(!IsPortableMode())
	{
		if(!m_ConfigPath.IsDirectory())
		{
			CreateDirectory(m_ConfigPath.AsNative().c_str(), 0);
		}
	}
	if(!(GetConfigPath() + P_("Components")).IsDirectory())
	{
		CreateDirectory((GetConfigPath() + P_("Components")).AsNative().c_str(), 0);
	}
	if(!(GetConfigPath() + P_("Components\\") + mpt::PathString::FromUnicode(mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture()))).IsDirectory())
	{
		CreateDirectory((GetConfigPath() + P_("Components\\") + mpt::PathString::FromUnicode(mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture()))).AsNative().c_str(), 0);
	}

	// Handle updates from old versions.

	if(!IsPortableMode())
	{

		// Move the config files if they're still in the old place.
		MoveConfigFile(P_("mptrack.ini"));
		MoveConfigFile(P_("plugin.cache"));
	
		// Import old tunings
		const mpt::PathString oldTunings = GetInstallPath() + P_("tunings\\");

		if(oldTunings.IsDirectory())
		{
			const mpt::PathString searchPattern = oldTunings + P_("*.*");
			WIN32_FIND_DATA FindFileData;
			HANDLE hFind;
			hFind = FindFirstFile(searchPattern.AsNative().c_str(), &FindFileData);
			if(hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					MoveConfigFile(mpt::PathString::FromNative(FindFileData.cFileName), P_("tunings\\"));
				} while(FindNextFile(hFind, &FindFileData) != 0);
			}
			FindClose(hFind);
			RemoveDirectory(oldTunings.AsNative().c_str());
		}

	}

}


#if !defined(MPT_BUILD_RETRO)

bool CTrackApp::CheckSystemSupport()
{
	const mpt::ustring lf = U_("\n");
	const mpt::ustring url = Build::GetURL(Build::Url::Download);
	if(!BuildVariants::ProcessorCanRunCurrentBuild())
	{
		mpt::ustring text;
		text += U_("Your CPU is too old to run this variant of OpenMPT.") + lf;
		text += U_("OpenMPT will exit now.") + lf;
		Reporting::Error(text, "OpenMPT");
		return false;
	}
	if(BuildVariants::IsKnownSystem() && !BuildVariants::SystemCanRunCurrentBuild())
	{
		mpt::ustring text;
		text += U_("Your system does not meet the minimum requirements for this variant of OpenMPT.") + lf;
		if(mpt::OS::Windows::IsOriginal())
		{
			text += U_("OpenMPT will exit now.") + lf;
		}
		Reporting::Error(text, "OpenMPT");
		if(mpt::OS::Windows::IsOriginal())
		{
			return false;
		} else
		{
			return true; // may work though
		}
	}
	return true;
}

#endif // !MPT_BUILD_RETRO


BOOL CTrackApp::InitInstanceEarly(CMPTCommandLineInfo &cmdInfo)
{
	// The first step of InitInstance, always executed without any crash handler.

	#ifndef UNICODE
		if(MessageBox(NULL,
			_T("STOP!!!") _T("\n")
			_T("This is an ANSI (as opposed to a UNICODE) build of OpenMPT.") _T("\n")
			_T("\n")
			_T("ANSI builds are NOT SUPPORTED and WILL CAUSE CORRUPTION of the OpenMPT configuration and exhibit other unintended behaviour.") _T("\n")
			_T("\n")
			_T("Please use an official build of OpenMPT or compile 'OpenMPT.sln' instead of 'OpenMPT-ANSI.sln'.") _T("\n")
			_T("\n")
			_T("Continue starting OpenMPT anyway?") _T("\n"),
			_T("OpenMPT"), MB_ICONSTOP | MB_YESNO| MB_DEFBUTTON2)
			!= IDYES)
		{
			ExitProcess(1);
		}
	#endif

	// Call the base class.
	// This is required for MFC RestartManager integration.
	if(!CWinApp::InitInstance())
	{
		return FALSE;
	}

	#if MPT_COMPILER_MSVC
		_CrtSetDebugFillThreshold(0); // Disable buffer filling in secure enhanced CRT functions.
	#endif

	// Initialize OLE MFC support
	BOOL oleinit = AfxOleInit();
	ASSERT(oleinit != FALSE); // no MPT_ASSERT here!

	// Parse command line for standard shell commands, DDE, file open
	ParseCommandLine(cmdInfo);

	// Set up paths to store configuration in
	SetupPaths(cmdInfo.m_portable);

	if(cmdInfo.m_sharedInstance && IPCWindow::SendToIPC(cmdInfo.m_fileNames))
	{
		ExitProcess(0);
	}

	// Initialize DocManager (for DDE)
	// requires mpt::PathString
	ASSERT(nullptr == m_pDocManager); // no MPT_ASSERT here!
	m_pDocManager = new CModDocManager();

	IPCWindow::Open(m_hInstance);

	if(IsDebuggerPresent() && cmdInfo.m_debugCrashHandler)
	{
		ExceptionHandler::useAnyCrashHandler = true;
		ExceptionHandler::useImplicitFallbackSEH = false;
		ExceptionHandler::useExplicitSEH = true;
		ExceptionHandler::handleStdTerminate = true;
		ExceptionHandler::handleMfcExceptions = true;
		ExceptionHandler::debugExceptionHandler = true;
	} else if(IsDebuggerPresent() || cmdInfo.m_noCrashHandler)
	{
		ExceptionHandler::useAnyCrashHandler = false;
		ExceptionHandler::useImplicitFallbackSEH = false;
		ExceptionHandler::useExplicitSEH = false;
		ExceptionHandler::handleStdTerminate = false;
		ExceptionHandler::handleMfcExceptions = false;
		ExceptionHandler::debugExceptionHandler = false;
	} else
	{
		ExceptionHandler::useAnyCrashHandler = true;
		ExceptionHandler::useImplicitFallbackSEH = true;
		ExceptionHandler::useExplicitSEH = true;
		ExceptionHandler::handleStdTerminate = true;
		ExceptionHandler::handleMfcExceptions = true;
		ExceptionHandler::debugExceptionHandler = false;
	}

	return TRUE;
}


BOOL CTrackApp::InitInstanceImpl(CMPTCommandLineInfo &cmdInfo)
{

	m_GuiThreadId = GetCurrentThreadId();

	mpt::log::Trace::SetThreadId(mpt::log::Trace::ThreadKindGUI, m_GuiThreadId);

	if(ExceptionHandler::useAnyCrashHandler)
	{
		ExceptionHandler::Register();
	}

	// Start loading
	BeginWaitCursor();

	MPT_LOG(LogInformation, "", U_("OpenMPT Start"));

	// create the tracker-global random device
	m_RD = std::make_unique<mpt::random_device>();
	// make the device available to non-tracker-only code
	mpt::set_global_random_device(m_RD.get());
	// create and seed the traker-global best PRNG with the random device
	m_PRNG = std::make_unique<mpt::thread_safe_prng<mpt::default_prng> >(mpt::make_prng<mpt::default_prng>(RandomDevice()));
	// make the best PRNG available to non-tracker-only code
	mpt::set_global_prng(m_PRNG.get());
	// additionally, seed the C rand() PRNG, just in case any third party library calls rand()
	mpt::rng::crand::reseed(RandomDevice());

	m_Gdiplus = std::make_unique<GdiplusRAII>();

	if(cmdInfo.m_noWine)
	{
		mpt::OS::Windows::PreventWineDetection();
	}

	#ifdef ENABLE_ASM
		CPU::Init();
		if(cmdInfo.m_noAssembly)
		{
			CPU::ProcSupport = 0;
		}
	#endif

	if(mpt::OS::Windows::IsWine())
	{
		SetWineVersion(std::make_shared<mpt::OS::Wine::VersionContext>());
	}

	// Create paths to store configuration in
	CreatePaths();

	m_pSettingsIniFile = new IniFileSettingsBackend(m_szConfigFileName);
	m_pSettings = new SettingsContainer(m_pSettingsIniFile);

	m_pDebugSettings = new DebugSettings(*m_pSettings);

	m_pTrackerSettings = new TrackerSettings(*m_pSettings);

	MPT_LOG(LogInformation, "", U_("OpenMPT settings initialized."));

	if(ExceptionHandler::useAnyCrashHandler)
	{
		ExceptionHandler::ConfigureSystemHandler();
	}

	m_pSongSettingsIniFile = new IniFileSettingsBackend(GetConfigPath() + P_("SongSettings.ini"));
	m_pSongSettings = new SettingsContainer(m_pSongSettingsIniFile);

	m_pComponentManagerSettings = new ComponentManagerSettings(TrackerSettings::Instance(), GetConfigPath());

	m_pPluginCache = new IniFileSettingsContainer(m_szPluginCacheFileName);

	// Load standard INI file options (without MRU)
	// requires SetupPaths+CreatePaths called
	LoadStdProfileSettings(0);

	// Set process priority class
	#ifndef _DEBUG
		SetPriorityClass(GetCurrentProcess(), TrackerSettings::Instance().MiscProcessPriorityClass);
	#endif

	// Dynamic DPI-awareness. Some users might want to disable DPI-awareness because of their DPI-unaware VST plugins.
	bool setDPI = false;
	// For Windows 10, Creators Update (1703) and newer
	{
		mpt::Library user32(mpt::LibraryPath::System(P_("user32")));
		if (user32.IsValid())
		{
			enum MPT_DPI_AWARENESS_CONTEXT
			{
				MPT_DPI_AWARENESS_CONTEXT_UNAWARE = -1,
				MPT_DPI_AWARENESS_CONTEXT_SYSTEM_AWARE = -2,
				MPT_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE = -3,
				MPT_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4,
				MPT_DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED = -5, // 1809 update and newer
			};
			using PSETPROCESSDPIAWARENESSCONTEXT = BOOL(WINAPI *)(HANDLE);
			PSETPROCESSDPIAWARENESSCONTEXT SetProcessDpiAwarenessContext = nullptr;
			if(user32.Bind(SetProcessDpiAwarenessContext, "SetProcessDpiAwarenessContext"))
			{
				if (TrackerSettings::Instance().highResUI)
				{
					setDPI = (SetProcessDpiAwarenessContext(HANDLE(MPT_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) == TRUE);
				} else
				{
					if (SetProcessDpiAwarenessContext(HANDLE(MPT_DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED)) == TRUE)
						setDPI = true;
					else
						setDPI = (SetProcessDpiAwarenessContext(HANDLE(MPT_DPI_AWARENESS_CONTEXT_UNAWARE)) == TRUE);
				}
			}
		}
	}
	// For Windows 8.1 and newer
	if(!setDPI)
	{
		mpt::Library shcore(mpt::LibraryPath::System(P_("SHCore")));
		if(shcore.IsValid())
		{
			using PSETPROCESSDPIAWARENESS = HRESULT (WINAPI *)(int);
			PSETPROCESSDPIAWARENESS SetProcessDPIAwareness = nullptr;
			if(shcore.Bind(SetProcessDPIAwareness, "SetProcessDpiAwareness"))
			{
				setDPI = (SetProcessDPIAwareness(TrackerSettings::Instance().highResUI ? 2 : 0) == S_OK);
			}
		}
	}
	// For Vista and newer
	if(!setDPI && TrackerSettings::Instance().highResUI)
	{
		mpt::Library user32(mpt::LibraryPath::System(P_("user32")));
		if(user32.IsValid())
		{
			using PSETPROCESSDPIAWARE = BOOL (WINAPI *)();
			PSETPROCESSDPIAWARE SetProcessDPIAware = nullptr;
			if(user32.Bind(SetProcessDPIAware, "SetProcessDPIAware"))
			{
				SetProcessDPIAware();
			}
		}
	}

	// create main MDI Frame window
	CMainFrame* pMainFrame = new CMainFrame();
	if(!pMainFrame->LoadFrame(IDR_MAINFRAME)) return FALSE;
	m_pMainWnd = pMainFrame;

	// Show splash screen
	if(cmdInfo.m_bShowSplash && TrackerSettings::Instance().m_ShowSplashScreen)
	{
		StartSplashScreen();
	}

	// create component manager
	ComponentManager::Init(*m_pComponentManagerSettings);

	// load components
	ComponentManager::Instance()->Startup();

	// Wine Support
	if(mpt::OS::Windows::IsWine())
	{
		WineIntegration::Initialize();
		WineIntegration::Load();
	}

	// Register document templates
	m_pModTemplate = new CModDocTemplate(
		IDR_MODULETYPE,
		RUNTIME_CLASS(CModDoc),
		RUNTIME_CLASS(CChildFrame), // custom MDI child frame
		RUNTIME_CLASS(CModControlView));
	AddDocTemplate(m_pModTemplate);

	// Load Midi Library
	ImportMidiConfig(theApp.GetSettings(), {}, true);

	// Enable DDE Execute open
	// requires m_pDocManager
	EnableShellOpen();

	// Enable drag/drop open
	m_pMainWnd->DragAcceptFiles();

	// Load sound APIs
	// requires TrackerSettings
	SoundDevice::SysInfo sysInfo = SoundDevice::SysInfo::Current();
	SoundDevice::AppInfo appInfo;
	appInfo.SetName(U_("OpenMPT"));
	appInfo.SetHWND(*m_pMainWnd);
	appInfo.BoostedThreadPriorityXP = TrackerSettings::Instance().SoundBoostedThreadPriority;
	appInfo.BoostedThreadMMCSSClassVista = TrackerSettings::Instance().SoundBoostedThreadMMCSSClass;
	appInfo.BoostedThreadRealtimePosix = TrackerSettings::Instance().SoundBoostedThreadRealtimePosix;
	appInfo.BoostedThreadNicenessPosix = TrackerSettings::Instance().SoundBoostedThreadNicenessPosix;
	appInfo.BoostedThreadRtprioPosix = TrackerSettings::Instance().SoundBoostedThreadRtprioPosix;
	appInfo.MaskDriverCrashes = TrackerSettings::Instance().SoundMaskDriverCrashes;
	appInfo.AllowDeferredProcessing = TrackerSettings::Instance().SoundAllowDeferredProcessing;
	m_pSoundDevicesManager = new SoundDevice::Manager(sysInfo, appInfo);
	m_pTrackerSettings->MigrateOldSoundDeviceSettings(*m_pSoundDevicesManager);

	// Set default note names
	CSoundFile::SetDefaultNoteNames();

	// Load DLS Banks
	if (!cmdInfo.m_noDls) LoadDefaultDLSBanks();

	// Initialize Plugins
	if (!cmdInfo.m_noPlugins) InitializeDXPlugins();

	// Initialize CMainFrame
	pMainFrame->Initialize();
	InitCommonControls();
	pMainFrame->m_InputHandler->UpdateMainMenu();

	// Dispatch commands specified on the command line
	if(cmdInfo.m_nShellCommand == CCommandLineInfo::FileNew)
	{
		// When not asked to open any existing file,
		// we do not want to open an empty new one on startup.
		cmdInfo.m_nShellCommand = CCommandLineInfo::FileNothing;
	}
	bool shellSuccess = false;
	if(cmdInfo.m_fileNames.empty())
	{
		shellSuccess = ProcessShellCommand(cmdInfo) != FALSE;
	} else
	{
		cmdInfo.m_nShellCommand = CCommandLineInfo::FileOpen;
		for(const auto &filename : cmdInfo.m_fileNames)
		{
			cmdInfo.m_strFileName = filename.ToCString();
			shellSuccess |= ProcessShellCommand(cmdInfo) != FALSE;
		}
	}
	if(!shellSuccess)
	{
		EndWaitCursor();
		StopSplashScreen();
		return FALSE;
	}

	pMainFrame->ShowWindow(m_nCmdShow);
	pMainFrame->UpdateWindow();

	EndWaitCursor();


	// Perform startup tasks.

#if !defined(MPT_BUILD_RETRO)
	// Check whether we are running the best build for the given system.
	if(!cmdInfo.m_noSysCheck)
	{
		if(!CheckSystemSupport())
		{
			StopSplashScreen();
			return FALSE;
		}
	}
#endif // !MPT_BUILD_RETRO

	if(TrackerSettings::Instance().FirstRun)
	{
		// On high-DPI devices, automatically upscale pattern font
		FontSetting font = TrackerSettings::Instance().patternFont;
		font.size = Clamp(Util::GetDPIy(m_pMainWnd->m_hWnd) / 96 - 1, 0, 9);
		TrackerSettings::Instance().patternFont = font;
		new WelcomeDlg(m_pMainWnd);
	} else
	{
#if !defined(MPT_BUILD_RETRO)
		bool deprecatedSoundDevice = GetSoundDevicesManager()->FindDeviceInfo(TrackerSettings::Instance().GetSoundDeviceIdentifier()).IsDeprecated();
		bool showSettings = deprecatedSoundDevice && !TrackerSettings::Instance().m_SoundDeprecatedDeviceWarningShown && (Reporting::Confirm(
			U_("You have currently selected a sound device which is deprecated. MME/WaveOut support will be removed in a future OpenMPT version.\n") +
			U_("The recommended sound device type is WASAPI.\n") +
			U_("Do you want to change your sound device settings now?"),
			U_("OpenMPT - Deprecated sound device")
			) == cnfYes);
		if(showSettings)
		{
			TrackerSettings::Instance().m_SoundDeprecatedDeviceWarningShown = true;
			m_pMainWnd->PostMessage(WM_COMMAND, ID_VIEW_OPTIONS);
		}
#endif // !MPT_BUILD_RETRO
	}

#ifdef ENABLE_TESTS
	if(!cmdInfo.m_noTests)
		Test::DoTests();
#endif

	if(TrackerSettings::Instance().m_SoundSettingsOpenDeviceAtStartup)
	{
		pMainFrame->InitPreview();
		pMainFrame->PreparePreview(NOTE_NOTECUT, 0);
		pMainFrame->PlayPreview();
	}

	if(!TrackerSettings::Instance().FirstRun)
	{
#if defined(MPT_ENABLE_UPDATE)
		if(CUpdateCheck::IsSuitableUpdateMoment())
		{
			CUpdateCheck::DoAutoUpdateCheck();
		}
#endif // MPT_ENABLE_UPDATE
	}

	return TRUE;
}


BOOL CTrackApp::InitInstance()
{
	CMPTCommandLineInfo cmdInfo;
	if(!InitInstanceEarly(cmdInfo))
	{
		return FALSE;
	}
	return InitInstanceLate(cmdInfo);
}


BOOL CTrackApp::InitInstanceLate(CMPTCommandLineInfo &cmdInfo)
{
	BOOL result = FALSE;
	if(ExceptionHandler::useExplicitSEH)
	{
		// https://support.microsoft.com/en-us/kb/173652
		__try
		{
			result = InitInstanceImpl(cmdInfo);
		} __except(ExceptionHandler::ExceptionFilter(GetExceptionInformation()))
		{
			std::abort();
		}
	} else
	{
		result = InitInstanceImpl(cmdInfo);
	}
	return result;
}


int CTrackApp::Run()
{
	int result = 255;
	if(ExceptionHandler::useExplicitSEH)
	{
		// https://support.microsoft.com/en-us/kb/173652
		__try
		{
			result = CWinApp::Run();
		} __except(ExceptionHandler::ExceptionFilter(GetExceptionInformation()))
		{
			std::abort();
		}
	} else
	{
		result = CWinApp::Run();
	}
	return result;
}


LRESULT CTrackApp::ProcessWndProcException(CException * e, const MSG * pMsg)
{
	if(ExceptionHandler::handleMfcExceptions)
	{
		LRESULT result = 0L; // as per documentation
		if(pMsg)
		{
			if(pMsg->message == WM_COMMAND)
			{
				result = (LRESULT)TRUE; // as per documentation
			}
		}
		if(dynamic_cast<CMemoryException*>(e))
		{
			e->ReportError();
			//ExceptionHandler::UnhandledMFCException(e, pMsg);
		} else
		{
			ExceptionHandler::UnhandledMFCException(e, pMsg);
		}
		return result;
	} else
	{
		return CWinApp::ProcessWndProcException(e, pMsg);
	}
}


int CTrackApp::ExitInstance()
{
	int result = 0;
	if(ExceptionHandler::useExplicitSEH)
	{
		// https://support.microsoft.com/en-us/kb/173652
		__try
		{
			result = ExitInstanceImpl();
		} __except(ExceptionHandler::ExceptionFilter(GetExceptionInformation()))
		{
			std::abort();
		}
	} else
	{
		result = ExitInstanceImpl();
	}
	return result;
}


int CTrackApp::ExitInstanceImpl()
{
	IPCWindow::Close();

	delete m_pSoundDevicesManager;
	m_pSoundDevicesManager = nullptr;
	ExportMidiConfig(theApp.GetSettings());
	SaveDefaultDLSBanks();
	for(auto &bank : gpDLSBanks)
	{
		delete bank;
	}
	gpDLSBanks.clear();

	// Uninitialize Plugins
	UninitializeDXPlugins();

	ComponentManager::Release();

	delete m_pPluginCache;
	m_pPluginCache = nullptr;
	delete m_pComponentManagerSettings;
	m_pComponentManagerSettings = nullptr;
	delete m_pTrackerSettings;
	m_pTrackerSettings = nullptr;
	delete m_pDebugSettings;
	m_pDebugSettings = nullptr;
	delete m_pSettings;
	m_pSettings = nullptr;
	delete m_pSettingsIniFile;
	m_pSettingsIniFile = nullptr;
	delete m_pSongSettings;
	m_pSongSettings = nullptr;
	delete m_pSongSettingsIniFile;
	m_pSongSettingsIniFile = nullptr;

	if(mpt::OS::Windows::IsWine())
	{
		SetWineVersion(nullptr);
	}

	m_Gdiplus.reset();

	mpt::set_global_prng(nullptr);
	m_PRNG.reset();
	mpt::set_global_random_device(nullptr);
	m_RD.reset();

	if(ExceptionHandler::useAnyCrashHandler)
	{
		ExceptionHandler::UnconfigureSystemHandler();
		ExceptionHandler::Unregister();
	}

	return CWinApp::ExitInstance();
}


////////////////////////////////////////////////////////////////////////////////
// App Messages


CModDoc *CTrackApp::NewDocument(MODTYPE newType)
{
	// Build from template
	if(newType == MOD_TYPE_NONE)
	{
		const mpt::PathString templateFile = TrackerSettings::Instance().defaultTemplateFile;
		if(TrackerSettings::Instance().defaultNewFileAction == nfDefaultTemplate && !templateFile.empty())
		{
			// Template file can be either a filename inside one of the preset and user TemplateModules folders, or a full path.
			const mpt::PathString dirs[] = { GetConfigPath() + P_("TemplateModules\\"), GetInstallPath() + P_("TemplateModules\\"), mpt::PathString() };
			for(const auto &dir : dirs)
			{
				if((dir + templateFile).IsFile())
				{
					if(CModDoc *modDoc = static_cast<CModDoc *>(m_pModTemplate->OpenTemplateFile(dir + templateFile)))
					{
						return modDoc;
					}
				}
			}
		}


		// Default module type
		newType = TrackerSettings::Instance().defaultModType;

		// Get active document to make the new module of the same type
		CModDoc *pModDoc = CMainFrame::GetMainFrame()->GetActiveDoc();
		if(pModDoc != nullptr && TrackerSettings::Instance().defaultNewFileAction == nfSameAsCurrent)
		{
			newType = pModDoc->GetSoundFile().GetBestSaveFormat();
		}
	}

	SetDefaultDocType(newType);
	return static_cast<CModDoc *>(m_pModTemplate->OpenDocumentFile(_T("")));
}


void CTrackApp::OpenModulesDialog(std::vector<mpt::PathString> &files, const mpt::PathString &overridePath)
{
	files.clear();

	std::string exts;
	for(const auto &ext : CSoundFile::GetSupportedExtensions(true))
	{
		exts += std::string("*.") + ext + std::string(";");
	}

	static int nFilterIndex = 0;
	FileDialog dlg = OpenFileDialog()
		.AllowMultiSelect()
		.ExtensionFilter("All Modules|" + exts + ";mod.*"
		"|"
		"Compressed Modules (*.mdz;*.s3z;*.xmz;*.itz;*.mo3)|*.mdz;*.s3z;*.xmz;*.itz;*.mdr;*.zip;*.rar;*.lha;*.pma;*.lzs;*.gz;*.mo3;*.oxm"
		"|"
		"ProTracker Modules (*.mod,*.nst)|*.mod;mod.*;*.mdz;*.nst;*.m15;*.stk;*.pt36|"
		"ScreamTracker Modules (*.s3m,*.stm)|*.s3m;*.stm;*.s3z|"
		"FastTracker Modules (*.xm)|*.xm;*.xmz|"
		"Impulse Tracker Modules (*.it)|*.it;*.itz|"
		"OpenMPT Modules (*.mptm)|*.mptm;*.mptmz|"
		"Other Modules (mtm,okt,mdl,669,far,...)|*.mtm;*.669;*.ult;*.wow;*.far;*.mdl;*.okt;*.dmf;*.ptm;*.med;*.ams;*.dbm;*.digi;*.dsm;*.dtm;*.umx;*.amf;*.psm;*.mt2;*.gdm;*.imf;*.itp;*.j2b;*.ice;*.st26;*.plm;*.stp;*.sfx;*.sfx2;*.symmod;*.mms;*.c67;*.mus;*.fmt|"
		"Wave Files (*.wav)|*.wav|"
		"MIDI Files (*.mid,*.rmi)|*.mid;*.rmi;*.smf|"
		"All Files (*.*)|*.*||")
		.WorkingDirectory(overridePath.empty() ? TrackerSettings::Instance().PathSongs.GetWorkingDir() : overridePath)
		.FilterIndex(&nFilterIndex);
	if(!dlg.Show()) return;

	if(overridePath.empty())
		TrackerSettings::Instance().PathSongs.SetWorkingDir(dlg.GetWorkingDirectory());

	files = dlg.GetFilenames();
}


void CTrackApp::OnFileOpen()
{
	FileDialog::PathList files;
	OpenModulesDialog(files);
	for(const auto &file : files)
	{
		OpenDocumentFile(file.ToCString());
	}
}


// App command to run the dialog
void CTrackApp::OnAppAbout()
{
	if (CAboutDlg::instance) return;
	CAboutDlg::instance = new CAboutDlg();
	CAboutDlg::instance->Create(IDD_ABOUTBOX, m_pMainWnd);
}


/////////////////////////////////////////////////////////////////////////////
// Splash Screen

class CSplashScreen: public CDialog
{
protected:
	std::unique_ptr<Gdiplus::Image> m_Image;

public:
	~CSplashScreen();
	BOOL OnInitDialog() override;
	void OnOK() override;
	void OnCancel() override { OnOK(); }
	void OnPaint();
	BOOL OnEraseBkgnd(CDC *) { return TRUE; }

	DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CSplashScreen, CDialog)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

static CSplashScreen *gpSplashScreen = NULL;

static DWORD64 gSplashScreenStartTime = 0;


CSplashScreen::~CSplashScreen()
{
	gpSplashScreen = nullptr;
}


void CSplashScreen::OnPaint()
{
	CPaintDC dc(this);
	Gdiplus::Graphics gfx(dc);

	CRect rect;
	GetClientRect(&rect);
	gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQuality);
	gfx.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
	gfx.DrawImage(m_Image.get(), 0, 0, rect.right, rect.bottom);

	CDialog::OnPaint();
}


BOOL CSplashScreen::OnInitDialog()
{
	CDialog::OnInitDialog();

	try
	{
		m_Image = GDIP::LoadPixelImage(GetResource(MAKEINTRESOURCE(IDB_SPLASHNOFOLDFIN), _T("PNG")));
	} catch(const bad_image &)
	{
		return FALSE;
	}

	CRect rect;
	GetWindowRect(&rect);
	const int width = Util::ScalePixels(m_Image->GetWidth(), m_hWnd) / 2;
	const int height = Util::ScalePixels(m_Image->GetHeight(), m_hWnd) / 2;
	SetWindowPos(nullptr,
		rect.left - ((width - rect.Width()) / 2),
		rect.top - ((height - rect.Height()) / 2),
		width,
		height,
		SWP_NOZORDER | SWP_NOCOPYBITS);

	return TRUE;
}


void CSplashScreen::OnOK()
{
	StopSplashScreen();
}


static void StartSplashScreen()
{
	if(!gpSplashScreen)
	{
		gpSplashScreen = new CSplashScreen();
		gpSplashScreen->Create(IDD_SPLASHSCREEN, theApp.m_pMainWnd);
		gpSplashScreen->ShowWindow(SW_SHOW);
		gpSplashScreen->UpdateWindow();
		gpSplashScreen->BeginWaitCursor();
		gSplashScreenStartTime = Util::GetTickCount64();
	}
}


static void StopSplashScreen()
{
	if(gpSplashScreen)
	{
		gpSplashScreen->EndWaitCursor();
		gpSplashScreen->DestroyWindow();
		delete gpSplashScreen;
		gpSplashScreen = nullptr;
	}
}


static void TimeoutSplashScreen()
{
	if(gpSplashScreen)
	{
		if(Util::GetTickCount64() - gSplashScreenStartTime > 100)
		{
			StopSplashScreen();
		}
	}
}


/////////////////////////////////////////////////////////////////////////////
// Idle-time processing

BOOL CTrackApp::OnIdle(LONG lCount)
{
	BOOL b = CWinApp::OnIdle(lCount);

	TimeoutSplashScreen();

	if(CMainFrame::GetMainFrame())
	{
		CMainFrame::GetMainFrame()->IdleHandlerSounddevice();
	}

	// Call plugins idle routine for open editor
	if (m_pPluginManager)
	{
		DWORD curTime = timeGetTime();
		//rewbs.vstCompliance: call @ 50Hz
		if (curTime - m_dwLastPluginIdleCall > 20 || curTime < m_dwLastPluginIdleCall)
		{
			m_pPluginManager->OnIdle();
			m_dwLastPluginIdleCall = curTime;
		}
	}

	return b;
}


/////////////////////////////////////////////////////////////////////////////
// DIB


RGBQUAD rgb2quad(COLORREF c)
{
	RGBQUAD r;
	r.rgbBlue = GetBValue(c);
	r.rgbGreen = GetGValue(c);
	r.rgbRed = GetRValue(c);
	r.rgbReserved = 0;
	return r;
}


void DibBlt(HDC hdc, int x, int y, int sizex, int sizey, int srcx, int srcy, MODPLUGDIB *lpdib)
{
	if (!lpdib) return;
	SetDIBitsToDevice(	hdc,
						x,
						y,
						sizex,
						sizey,
						srcx,
						lpdib->bmiHeader.biHeight - srcy - sizey,
						0,
						lpdib->bmiHeader.biHeight,
						lpdib->lpDibBits,
						(LPBITMAPINFO)lpdib,
						DIB_RGB_COLORS);
}


MODPLUGDIB *LoadDib(LPCTSTR lpszName)
{
	mpt::const_byte_span data = GetResource(lpszName, RT_BITMAP);
	if(!data.data())
	{
		return nullptr;
	}
	LPBITMAPINFO p = (LPBITMAPINFO)data.data();
	MODPLUGDIB *pmd = new MODPLUGDIB;
	pmd->bmiHeader = p->bmiHeader;
	for (int i=0; i<16; i++) pmd->bmiColors[i] = p->bmiColors[i];
	LPBYTE lpDibBits = (LPBYTE)p;
	lpDibBits += p->bmiHeader.biSize + 16 * sizeof(RGBQUAD);
	pmd->lpDibBits = lpDibBits;
	return pmd;
}

int DrawTextT(HDC hdc, const wchar_t *lpchText, int cchText, LPRECT lprc, UINT format)
{
	return ::DrawTextW(hdc, lpchText, cchText, lprc, format);
}

int DrawTextT(HDC hdc, const char *lpchText, int cchText, LPRECT lprc, UINT format)
{
	return ::DrawTextA(hdc, lpchText, cchText, lprc, format);
}

template<typename Tchar>
static void DrawButtonRectImpl(HDC hdc, CRect rect, const Tchar *lpszText, bool disabled, bool pushed, DWORD textFlags, uint32 topMargin)
{
	int width = Util::ScalePixels(1, WindowFromDC(hdc));
	if(width != 1)
	{
		// Draw "real" buttons in Hi-DPI mode
		DrawFrameControl(hdc, rect, DFC_BUTTON, pushed ? (DFCS_PUSHED | DFCS_BUTTONPUSH) : DFCS_BUTTONPUSH);
	} else
	{
		const auto colorHighlight = GetSysColor(COLOR_BTNHIGHLIGHT), colorShadow = GetSysColor(COLOR_BTNSHADOW);
		auto oldpen = SelectPen(hdc, GetStockObject(DC_PEN));
		::SetDCPenColor(hdc, pushed ? colorShadow : colorHighlight);
		::FillRect(hdc, rect, GetSysColorBrush(COLOR_BTNFACE));
		::MoveToEx(hdc, rect.left, rect.bottom - 1, nullptr);
		::LineTo(hdc, rect.left, rect.top);
		::LineTo(hdc, rect.right - 1, rect.top);
		::SetDCPenColor(hdc, pushed ? colorHighlight : colorShadow);
		::LineTo(hdc, rect.right - 1, rect.bottom - 1);
		::LineTo(hdc, rect.left, rect.bottom - 1);
		SelectPen(hdc, oldpen);
	}
	
	if(lpszText && lpszText[0])
	{
		rect.DeflateRect(width, width);
		if(pushed)
		{
			rect.top += width;
			rect.left += width;
		}
		::SetTextColor(hdc, GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
		::SetBkMode(hdc, TRANSPARENT);
		rect.top += topMargin;
		auto oldfont = SelectFont(hdc, CMainFrame::GetGUIFont());
		DrawTextT(hdc, lpszText, -1, &rect, textFlags | DT_SINGLELINE | DT_NOPREFIX);
		SelectFont(hdc, oldfont);
	}
}


void DrawButtonRect(HDC hdc, const RECT *lpRect, LPCSTR lpszText, BOOL bDisabled, BOOL bPushed, DWORD dwFlags, uint32 topMargin)
{
	DrawButtonRectImpl(hdc, *lpRect, lpszText, bDisabled, bPushed, dwFlags, topMargin);
}


void DrawButtonRect(HDC hdc, const RECT *lpRect, LPCWSTR lpszText, BOOL bDisabled, BOOL bPushed, DWORD dwFlags, uint32 topMargin)
{
	DrawButtonRectImpl(hdc, *lpRect, lpszText, bDisabled, bPushed, dwFlags, topMargin);
}



//////////////////////////////////////////////////////////////////////////////////
// Misc functions


void ErrorBox(UINT nStringID, CWnd *parent)
{
	CString str;
	BOOL resourceLoaded = str.LoadString(nStringID);
	if(!resourceLoaded)
	{
		str.Format(_T("Resource string %u not found."), nStringID);
	}
	MPT_ASSERT(resourceLoaded);
	Reporting::CustomNotification(str, _T("Error!"), MB_OK | MB_ICONERROR, parent);
}


CString GetWindowTextString(const CWnd &wnd)
{
	CString result;
	wnd.GetWindowText(result);
	return result;
}


mpt::ustring GetWindowTextUnicode(const CWnd &wnd)
{
	return mpt::ToUnicode(GetWindowTextString(wnd));
}


////////////////////////////////////////////////////////////////////////////////
// CFastBitmap 8-bit output / 4-bit input
// useful for lots of small blits with color mapping
// combined in one big blit

void CFastBitmap::Init(MODPLUGDIB *lpTextDib)
{
	m_nBlendOffset = 0;
	m_pTextDib = lpTextDib;
	MemsetZero(m_Dib.bmiHeader);
	m_nTextColor = 0;
	m_nBkColor = 1;
	m_Dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	m_Dib.bmiHeader.biWidth = 0;	// Set later
	m_Dib.bmiHeader.biHeight = 0;	// Ditto
	m_Dib.bmiHeader.biPlanes = 1;
	m_Dib.bmiHeader.biBitCount = 8;
	m_Dib.bmiHeader.biCompression = BI_RGB;
	m_Dib.bmiHeader.biSizeImage = 0;
	m_Dib.bmiHeader.biXPelsPerMeter = 96;
	m_Dib.bmiHeader.biYPelsPerMeter = 96;
	m_Dib.bmiHeader.biClrUsed = 0;
	m_Dib.bmiHeader.biClrImportant = 256; // MAX_MODPALETTECOLORS;
	m_n4BitPalette[0] = (BYTE)m_nTextColor;
	m_n4BitPalette[4] = MODCOLOR_SEPSHADOW;
	m_n4BitPalette[12] = MODCOLOR_SEPFACE;
	m_n4BitPalette[14] = MODCOLOR_SEPHILITE;
	m_n4BitPalette[15] = (BYTE)m_nBkColor;
}


void CFastBitmap::Blit(HDC hdc, int x, int y, int cx, int cy)
{
	SetDIBitsToDevice(	hdc,
						x,
						y,
						cx,
						cy,
						0,
						m_Dib.bmiHeader.biHeight - cy,
						0,
						m_Dib.bmiHeader.biHeight,
						&m_Dib.DibBits[0],
						(LPBITMAPINFO)&m_Dib,
						DIB_RGB_COLORS);
}


void CFastBitmap::SetColor(UINT nIndex, COLORREF cr)
{
	if (nIndex < 256)
	{
		m_Dib.bmiColors[nIndex].rgbRed = GetRValue(cr);
		m_Dib.bmiColors[nIndex].rgbGreen = GetGValue(cr);
		m_Dib.bmiColors[nIndex].rgbBlue = GetBValue(cr);
	}
}


void CFastBitmap::SetAllColors(UINT nBaseIndex, UINT nColors, COLORREF *pcr)
{
	for (UINT i=0; i<nColors; i++)
	{
		SetColor(nBaseIndex+i, pcr[i]);
	}
}


void CFastBitmap::SetBlendColor(COLORREF cr)
{
	UINT r = GetRValue(cr);
	UINT g = GetGValue(cr);
	UINT b = GetBValue(cr);
	for (UINT i=0; i<BLEND_OFFSET; i++)
	{
		UINT m = (m_Dib.bmiColors[i].rgbRed >> 2)
				+ (m_Dib.bmiColors[i].rgbGreen >> 1)
				+ (m_Dib.bmiColors[i].rgbBlue >> 2);
		m_Dib.bmiColors[i|BLEND_OFFSET].rgbRed = static_cast<BYTE>((m + r)>>1);
		m_Dib.bmiColors[i|BLEND_OFFSET].rgbGreen = static_cast<BYTE>((m + g)>>1);
		m_Dib.bmiColors[i|BLEND_OFFSET].rgbBlue = static_cast<BYTE>((m + b)>>1);
	}
}


// Monochrome 4-bit bitmap (0=text, !0 = back)
void CFastBitmap::TextBlt(int x, int y, int cx, int cy, int srcx, int srcy, MODPLUGDIB *lpdib)
{
	const uint8 *psrc;
	BYTE *pdest;
	UINT x1, x2;
	int srcwidth, srcinc;

	m_n4BitPalette[0] = (BYTE)m_nTextColor;
	m_n4BitPalette[15] = (BYTE)m_nBkColor;
	if (x < 0)
	{
		cx += x;
		x = 0;
	}
	if (y < 0)
	{
		cy += y;
		y = 0;
	}
	if ((x >= m_Dib.bmiHeader.biWidth) || (y >= m_Dib.bmiHeader.biHeight)) return;
	if (x+cx >= m_Dib.bmiHeader.biWidth) cx = m_Dib.bmiHeader.biWidth - x;
	if (y+cy >= m_Dib.bmiHeader.biHeight) cy = m_Dib.bmiHeader.biHeight - y;
	if (!lpdib) lpdib = m_pTextDib;
	if ((cx <= 0) || (cy <= 0) || (!lpdib)) return;
	srcwidth = (lpdib->bmiHeader.biWidth+1) >> 1;
	srcinc = srcwidth;
	if (((int)lpdib->bmiHeader.biHeight) > 0)
	{
		srcy = lpdib->bmiHeader.biHeight - 1 - srcy;
		srcinc = -srcinc;
	}
	x1 = srcx & 1;
	x2 = x1 + cx;
	pdest = &m_Dib.DibBits[((m_Dib.bmiHeader.biHeight - 1 - y) << m_nXShiftFactor) + x];
	psrc = lpdib->lpDibBits + (srcx >> 1) + (srcy * srcwidth);
	for (int iy=0; iy<cy; iy++)
	{
		uint8 *p = pdest;
		UINT ix = x1;
		if (ix&1)
		{
			UINT b = psrc[ix >> 1];
			*p++ = m_n4BitPalette[b & 0x0F]+m_nBlendOffset;
			ix++;
		}
		while (ix+1 < x2)
		{
			UINT b = psrc[ix >> 1];
			p[0] = m_n4BitPalette[b >> 4]+m_nBlendOffset;
			p[1] = m_n4BitPalette[b & 0x0F]+m_nBlendOffset;
			ix+=2;
			p+=2;
		}
		if (x2&1)
		{
			UINT b = psrc[ix >> 1];
			*p++ = m_n4BitPalette[b >> 4]+m_nBlendOffset;
		}
		pdest -= m_Dib.bmiHeader.biWidth;
		psrc += srcinc;
	}
}


void CFastBitmap::SetSize(int x, int y)
{
	if(x > 4)
	{
		// Compute the required shift factor for obtaining a power-of-two bitmap width
		m_nXShiftFactor = 1;
		x--;
		while(x >>= 1)
		{
			m_nXShiftFactor++;
		}
	} else
	{
		// Bitmaps rows are aligned to 4 bytes, so let this bitmap be exactly 4 pixels wide.
		m_nXShiftFactor = 2;
	}

	x = (1 << m_nXShiftFactor);
	if(m_Dib.DibBits.size() != static_cast<size_t>(y << m_nXShiftFactor)) m_Dib.DibBits.resize(y << m_nXShiftFactor);
	m_Dib.bmiHeader.biWidth = x;
	m_Dib.bmiHeader.biHeight = y;
}


///////////////////////////////////////////////////////////////////////////////////
//
// DirectX Plugins
//

void CTrackApp::InitializeDXPlugins()
{
	m_pPluginManager = new CVstPluginManager;
	const size_t numPlugins = GetSettings().Read<int32>(U_("VST Plugins"), U_("NumPlugins"), 0);

	bool maskCrashes = TrackerSettings::Instance().BrokenPluginsWorkaroundVSTMaskAllCrashes;

	std::vector<VSTPluginLib *> nonFoundPlugs;
	const mpt::PathString failedPlugin = GetSettings().Read<mpt::PathString>(U_("VST Plugins"), U_("FailedPlugin"), P_(""));

	CDialog pluginScanDlg;
	CWnd *textWnd = nullptr;
	DWORD64 scanStart = Util::GetTickCount64();

	// Read tags for built-in plugins
	for(auto plug : *m_pPluginManager)
	{
		mpt::ustring key = MPT_UFORMAT("Plugin{}{}.Tags")(mpt::ufmt::HEX0<8>(plug->pluginId1), mpt::ufmt::HEX0<8>(plug->pluginId2));
		plug->tags = GetSettings().Read<mpt::ustring>(U_("VST Plugins"), key, mpt::ustring());
	}

	// Restructured plugin cache
	if(TrackerSettings::Instance().PreviousSettingsVersion < MPT_V("1.27.00.15"))
	{
		DeleteFile(m_szPluginCacheFileName.AsNative().c_str());
		GetPluginCache().ForgetAll();
	}

	m_pPluginManager->reserve(numPlugins);
	auto plugIDFormat = MPT_UFORMAT("Plugin{}");
	auto scanFormat = MPT_CFORMAT("Scanning Plugin {} / {}...\n{}");
	auto tagFormat = MPT_UFORMAT("Plugin{}.Tags");
	for(size_t plug = 0; plug < numPlugins; plug++)
	{
		mpt::PathString plugPath = GetSettings().Read<mpt::PathString>(U_("VST Plugins"), plugIDFormat(plug), mpt::PathString());
		if(!plugPath.empty())
		{
			plugPath = PathInstallRelativeToAbsolute(plugPath);

			if(!pluginScanDlg.m_hWnd && Util::GetTickCount64() >= scanStart + 2000)
			{
				// If this is taking too long, show the user what they're waiting for.
				pluginScanDlg.Create(IDD_SCANPLUGINS, gpSplashScreen);
				pluginScanDlg.ShowWindow(SW_SHOW);
				pluginScanDlg.CenterWindow(gpSplashScreen);
				textWnd = pluginScanDlg.GetDlgItem(IDC_SCANTEXT);
			} else if(pluginScanDlg.m_hWnd && Util::GetTickCount64() >= scanStart + 30)
			{
				textWnd->SetWindowText(scanFormat(plug + 1, numPlugins + 1, plugPath));
				MSG msg;
				while(::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
				{
					::TranslateMessage(&msg);
					::DispatchMessage(&msg);
				}
				scanStart = Util::GetTickCount64();
			}

			if(plugPath == failedPlugin)
			{
				GetSettings().Remove(U_("VST Plugins"), U_("FailedPlugin"));
				const CString text = _T("The following plugin has previously crashed OpenMPT during initialisation:\n\n") + failedPlugin.ToCString() + _T("\n\nDo you still want to load it?");
				if(Reporting::Confirm(text, false, true, &pluginScanDlg) == cnfNo)
				{
					continue;
				}
			}

			mpt::ustring plugTags = GetSettings().Read<mpt::ustring>(U_("VST Plugins"), tagFormat(plug), mpt::ustring());

			bool plugFound = true;
			VSTPluginLib *lib = m_pPluginManager->AddPlugin(plugPath, maskCrashes, plugTags, true, &plugFound);
			if(!plugFound && lib != nullptr)
			{
				nonFoundPlugs.push_back(lib);
			}
			if(lib != nullptr && lib->libraryName == P_("MIDI Input Output") && lib->pluginId1 == PLUGMAGIC('V','s','t','P') && lib->pluginId2 == PLUGMAGIC('M','M','I','D'))
			{
				// This appears to be an old version of our MIDI I/O plugin, which is now built right into the main executable.
				m_pPluginManager->RemovePlugin(lib);
			}
		}
	}
	GetPluginCache().Flush();
	if(pluginScanDlg.m_hWnd)
	{
		pluginScanDlg.DestroyWindow();
	}
	if(!nonFoundPlugs.empty())
	{
		PlugNotFoundDialog(nonFoundPlugs, nullptr).DoModal();
	}
}


void CTrackApp::UninitializeDXPlugins()
{
	if(!m_pPluginManager) return;

#ifndef NO_PLUGINS

	size_t plugIndex = 0;
	for(auto plug : *m_pPluginManager)
	{
		if(!plug->isBuiltIn)
		{
			mpt::PathString plugPath = plug->dllPath;
			if(theApp.IsPortableMode())
			{
				plugPath = PathAbsoluteToInstallRelative(plugPath);
			}
			theApp.GetSettings().Write<mpt::PathString>(U_("VST Plugins"), MPT_UFORMAT("Plugin{}")(plugIndex), plugPath);

			theApp.GetSettings().Write(U_("VST Plugins"), MPT_UFORMAT("Plugin{}.Tags")(plugIndex), plug->tags);

			plugIndex++;
		} else
		{
			mpt::ustring key = MPT_UFORMAT("Plugin{}{}.Tags")(mpt::ufmt::HEX0<8>(plug->pluginId1), mpt::ufmt::HEX0<8>(plug->pluginId2));
			theApp.GetSettings().Write(U_("VST Plugins"), key, plug->tags);
		}
	}
	theApp.GetSettings().Write(U_("VST Plugins"), U_("NumPlugins"), static_cast<uint32>(plugIndex));
#endif // NO_PLUGINS

	delete m_pPluginManager;
	m_pPluginManager = nullptr;
}


///////////////////////////////////////////////////////////////////////////////////
// Internet-related functions

bool CTrackApp::OpenURL(const char *url)
{
	if(!url) return false;
	return OpenURL(mpt::PathString::FromUTF8(url));
}

bool CTrackApp::OpenURL(const std::string &url)
{
	return OpenURL(mpt::PathString::FromUTF8(url));
}

bool CTrackApp::OpenURL(const CString &url)
{
	return OpenURL(mpt::ToUnicode(url));
}

bool CTrackApp::OpenURL(const mpt::ustring &url)
{
	return OpenURL(mpt::PathString::FromUnicode(url));
}

bool CTrackApp::OpenURL(const mpt::PathString &lpszURL)
{
	if(!lpszURL.empty() && theApp.m_pMainWnd)
	{
		if(reinterpret_cast<INT_PTR>(ShellExecute(
			theApp.m_pMainWnd->m_hWnd,
			_T("open"),
			lpszURL.AsNative().c_str(),
			NULL,
			NULL,
			SW_SHOW)) >= 32)
		{
			return true;
		}
	}
	return false;
}


CString CTrackApp::GetResamplingModeName(ResamplingMode mode, int length, bool addTaps)
{
	CString result;
	switch(mode)
	{
	case SRCMODE_NEAREST:
		result = (length > 1) ? _T("No Interpolation") : _T("None") ;
		break;
	case SRCMODE_LINEAR:
		result = _T("Linear");
		break;
	case SRCMODE_CUBIC:
		result = _T("Cubic");
		break;
	case SRCMODE_SINC8:
		result = _T("Sinc");
		break;
	case SRCMODE_SINC8LP:
		result = _T("Sinc");
		break;
	default:
		MPT_ASSERT_NOTREACHED();
		break;
	}
	if(Resampling::HasAA(mode))
	{
		result += (length > 1) ? _T(" + Low-Pass") : _T(" + LP");
	}
	if(addTaps)
	{
		result += MPT_CFORMAT(" ({} tap{})")(Resampling::Length(mode), (Resampling::Length(mode) != 1) ? CString(_T("s")) : CString(_T("")));
	}
	return result;
}


mpt::ustring CTrackApp::GetFriendlyMIDIPortName(const mpt::ustring &deviceName, bool isInputPort, bool addDeviceName)
{
	auto friendlyName = GetSettings().Read<mpt::ustring>(isInputPort ? U_("MIDI Input Ports") : U_("MIDI Output Ports"), deviceName, deviceName);
	if(friendlyName.empty())
		return deviceName;
	else if(addDeviceName && friendlyName != deviceName)
		return friendlyName + UL_(" (") + deviceName + UL_(")");
	else
		return friendlyName;
}


CString CTrackApp::GetFriendlyMIDIPortName(const CString &deviceName, bool isInputPort, bool addDeviceName)
{
	return mpt::ToCString(GetFriendlyMIDIPortName(mpt::ToUnicode(deviceName), isInputPort, addDeviceName));
}


OPENMPT_NAMESPACE_END
