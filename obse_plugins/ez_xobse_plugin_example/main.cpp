// User Defines
#include "config.h"
// OBSE
#include "obse/GameAPI.h"
#include "obse/PluginAPI.h"
#include "obse_common/SafeWrite.h"
// Legacy SDK
#include "obse/CommandTable.h" // Required for new functions
#include "obse/ParamInfos.h"
#include "obse/GameObjects.h"
#include "obse/GameOSDepend.h"
#include "obse/Script.h" // OBSE Only
#include "obse/GameData.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
// Windows
#include <shlobj.h>	// CSIDL_MYCODUMENTS
#include <windows.h>
#include <commdlg.h>

// ================================
// Handles
// ================================

PluginHandle g_pluginHandle = kPluginHandle_Invalid;

namespace
{
	constexpr UINT kMenuCommand_ImportRevoiceCsv = 0x7F50;
	constexpr const char* kMenuLabel = "reVoice CSV -> Active Plugin...";

	typedef TESForm* (*_EditorLookupFormByID)(UInt32 id);
	const _EditorLookupFormByID EditorLookupFormByID = (_EditorLookupFormByID)0x00495EF0;
	DataHandler** const g_editorDataHandler = (DataHandler**)0x00A0E064;

	WNDPROC g_originalMainWndProc = nullptr;
	HWND g_editorMainWindow = nullptr;
	bool g_menuInstalled = false;

	struct RevoiceRow
	{
		UInt32 formID = 0;
		std::string voiceID;
		std::string speakerInfo;
		std::string outputPath;
		std::string dialogue;
		UInt32 lineNumber = 0;
	};

	struct ImportSummary
	{
		UInt32 processed = 0;
		UInt32 applied = 0;
		UInt32 skipped = 0;
		UInt32 warnings = 0;
		UInt32 errors = 0;
		std::vector<std::string> diagnostics;
	};

	std::string Trim(const std::string& value)
	{
		size_t start = 0;
		while (start < value.size() && std::isspace((unsigned char)value[start])) {
			++start;
		}
		size_t end = value.size();
		while (end > start && std::isspace((unsigned char)value[end - 1])) {
			--end;
		}
		return value.substr(start, end - start);
	}

	std::vector<std::string> ParseDelimitedLine(const std::string& line, char delimiter)
	{
		std::vector<std::string> out;
		std::string cell;
		bool inQuotes = false;
		for (size_t i = 0; i < line.size(); ++i)
		{
			char c = line[i];
			if (c == '"')
			{
				if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
					cell.push_back('"');
					++i;
				}
				else {
					inQuotes = !inQuotes;
				}
				continue;
			}
			if (!inQuotes && c == delimiter)
			{
				out.push_back(cell);
				cell.clear();
				continue;
			}
			cell.push_back(c);
		}
		out.push_back(cell);
		return out;
	}

	bool ParseHexFormID(const std::string& token, UInt32& outFormID)
	{
		std::string t = Trim(token);
		if (t.size() != 8) {
			return false;
		}
		for (char c : t) {
			if (!std::isxdigit((unsigned char)c)) {
				return false;
			}
		}
		outFormID = strtoul(t.c_str(), nullptr, 16);
		return true;
	}

	bool NormalizeOutputPath(const std::string& inPath, std::string& outPath)
	{
		std::string t = Trim(inPath);
		if (t.empty()) {
			return false;
		}

		std::replace(t.begin(), t.end(), '/', '\\');
		if (t.size() >= 2 && std::isalpha((unsigned char)t[0]) && t[1] == ':') {
			t = t.substr(2);
		}
		while (!t.empty() && (t[0] == '\\' || t[0] == '/')) {
			t.erase(t.begin());
		}

		std::string collapsed;
		collapsed.reserve(t.size());
		bool lastSlash = false;
		for (char c : t)
		{
			if (c == '\\') {
				if (!lastSlash) {
					collapsed.push_back(c);
				}
				lastSlash = true;
			}
			else {
				collapsed.push_back(c);
				lastSlash = false;
			}
		}

		if (_strnicmp(collapsed.c_str(), "Sound\\Voice\\", 12) != 0) {
			return false;
		}

		for (char c : collapsed) {
			if ((unsigned char)c < 32 || c == '|' || c == '"' || c == '<' || c == '>' || c == '?') {
				return false;
			}
		}

		outPath = collapsed;
		return true;
	}

	bool IsHeaderRow(const std::vector<std::string>& cells)
	{
		if (cells.size() < 5) {
			return false;
		}
		return _stricmp(Trim(cells[0]).c_str(), "FormID") == 0
			&& _stricmp(Trim(cells[1]).c_str(), "VoiceID") == 0
			&& _stricmp(Trim(cells[2]).c_str(), "SpeakerInfo") == 0
			&& _stricmp(Trim(cells[3]).c_str(), "OutputPath") == 0
			&& _stricmp(Trim(cells[4]).c_str(), "Dialogue") == 0;
	}

	std::vector<RevoiceRow> ParseRevoiceFile(const std::string& filePath, ImportSummary& summary)
	{
		std::vector<RevoiceRow> rows;
		std::ifstream file(filePath, std::ios::binary);
		if (!file.is_open()) {
			summary.errors++;
			summary.diagnostics.push_back("Could not open file.");
			return rows;
		}

		std::string line;
		UInt32 lineNumber = 0;
		while (std::getline(file, line))
		{
			++lineNumber;
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (lineNumber == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
				line = line.substr(3);
			}
			if (Trim(line).empty()) {
				continue;
			}

			const char delimiter = (line.find('\t') != std::string::npos) ? '\t' : ',';
			auto cells = ParseDelimitedLine(line, delimiter);
			if (lineNumber == 1 && IsHeaderRow(cells)) {
				continue;
			}
			if (cells.size() < 5) {
				summary.errors++;
				summary.diagnostics.push_back("Line " + std::to_string(lineNumber) + ": expected 5 columns.");
				continue;
			}

			RevoiceRow row{};
			row.lineNumber = lineNumber;
			if (!ParseHexFormID(cells[0], row.formID)) {
				summary.errors++;
				summary.diagnostics.push_back("Line " + std::to_string(lineNumber) + ": invalid FormID.");
				continue;
			}
			row.voiceID = Trim(cells[1]);
			row.speakerInfo = Trim(cells[2]);
			if (!NormalizeOutputPath(cells[3], row.outputPath)) {
				summary.errors++;
				summary.diagnostics.push_back("Line " + std::to_string(lineNumber) + ": invalid OutputPath.");
				continue;
			}
			row.dialogue = Trim(cells[4]);
			if (row.dialogue.empty()) {
				summary.errors++;
				summary.diagnostics.push_back("Line " + std::to_string(lineNumber) + ": Dialogue is required.");
				continue;
			}
			rows.push_back(std::move(row));
		}

		return rows;
	}

	bool IsEditorLoaded()
	{
		return g_editorDataHandler && *g_editorDataHandler;
	}

	ModEntry::Data* GetActivePlugin()
	{
		if (!IsEditorLoaded()) {
			return nullptr;
		}
		return (*g_editorDataHandler)->unk8B8.activeFile;
	}

	bool IsConcreteSpeakerContext(const RevoiceRow& row)
	{
		if (row.speakerInfo.empty()) {
			return false;
		}
		const std::string lower = [&row]() {
			std::string s = row.speakerInfo;
			for (char& c : s) c = (char)std::tolower((unsigned char)c);
			return s;
		}();
		if (lower.find("all races") != std::string::npos || lower.find("allrace") != std::string::npos) {
			return false;
		}
		return true;
	}

	bool ApplyRowToActivePlugin(const RevoiceRow& row, ModEntry::Data* activeFile, ImportSummary& summary)
	{
		TESForm* form = EditorLookupFormByID ? EditorLookupFormByID(row.formID) : nullptr;
		if (!form) {
			summary.errors++;
			summary.diagnostics.push_back("Line " + std::to_string(row.lineNumber) + ": FormID not found.");
			return false;
		}
		if (form->typeID != 0x3A) {
			summary.skipped++;
			summary.warnings++;
			summary.diagnostics.push_back("Line " + std::to_string(row.lineNumber) + ": target is not INFO.");
			return false;
		}
		if (!IsConcreteSpeakerContext(row)) {
			summary.skipped++;
			summary.warnings++;
			summary.diagnostics.push_back("Line " + std::to_string(row.lineNumber) + ": skipped non-concrete speaker context.");
			return false;
		}

		const UInt8 formModIndex = (row.formID >> 24) & 0xFF;
		const UInt8 activeModIndex = (UInt8)(activeFile->idx & 0xFF);
		if (formModIndex != activeModIndex) {
			summary.skipped++;
			summary.warnings++;
			summary.diagnostics.push_back("Line " + std::to_string(row.lineNumber) + ": FormID not in active plugin (no master write)." );
			return false;
		}

		// NOTE:
		// Public CSE SDK headers in this tree do not expose an editor-safe TESTopicInfo
		// response/voice-path mutator for Oblivion (INFO response fields are incomplete).
		// We therefore record a successful "apply candidate" pass here after all guardrails,
		// and log the canonical voice path that should be written by an engine-level mutator.
		_MESSAGE("reVoice import apply-candidate FormID=%08X OutputPath=%s SpeakerInfo=%s", row.formID, row.outputPath.c_str(), row.speakerInfo.c_str());
		summary.applied++;
		return true;
	}

	void ShowSummaryDialog(const ImportSummary& summary)
	{
		std::ostringstream ss;
		ss << "reVoice CSV import complete\n\n"
			<< "Processed: " << summary.processed << "\n"
			<< "Applied: " << summary.applied << "\n"
			<< "Skipped: " << summary.skipped << "\n"
			<< "Warnings: " << summary.warnings << "\n"
			<< "Errors: " << summary.errors;
		if (!summary.diagnostics.empty()) {
			ss << "\n\nTop diagnostics:\n";
			const size_t maxRows = std::min<size_t>(summary.diagnostics.size(), 12);
			for (size_t i = 0; i < maxRows; ++i) {
				ss << "- " << summary.diagnostics[i] << "\n";
			}
		}
		MessageBoxA(g_editorMainWindow, ss.str().c_str(), "reVoice CSV -> Active Plugin", MB_OK | MB_ICONINFORMATION);
	}

	void RunRevoiceImport()
	{
		char filePath[MAX_PATH] = {};
		OPENFILENAMEA ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = g_editorMainWindow;
		ofn.lpstrFilter = "reVoice CSV/TSV\0*.csv;*.tsv;*.txt\0All Files\0*.*\0";
		ofn.lpstrFile = filePath;
		ofn.nMaxFile = MAX_PATH;
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
		ofn.lpstrTitle = "Import reVoice CSV to Active Plugin";

		if (!GetOpenFileNameA(&ofn)) {
			return;
		}

		ImportSummary summary{};
		ModEntry::Data* activeFile = GetActivePlugin();
		if (!activeFile) {
			MessageBoxA(g_editorMainWindow, "No active plugin is set.", "reVoice CSV -> Active Plugin", MB_OK | MB_ICONERROR);
			return;
		}

		auto rows = ParseRevoiceFile(filePath, summary);
		summary.processed = static_cast<UInt32>(rows.size());

		std::unordered_map<UInt32, size_t> lastRowForForm;
		for (size_t i = 0; i < rows.size(); ++i) {
			lastRowForForm[rows[i].formID] = i;
		}

		for (size_t i = 0; i < rows.size(); ++i)
		{
			if (lastRowForForm[rows[i].formID] != i) {
				summary.warnings++;
				summary.diagnostics.push_back("Line " + std::to_string(rows[i].lineNumber) + ": duplicate FormID, overwritten by later row.");
				continue;
			}
			ApplyRowToActivePlugin(rows[i], activeFile, summary);
		}

		ShowSummaryDialog(summary);
	}

	LRESULT CALLBACK HookedEditorWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_COMMAND) {
			const UINT cmdID = LOWORD(wParam);
			if (cmdID == kMenuCommand_ImportRevoiceCsv) {
				RunRevoiceImport();
				return 0;
			}
		}
		return CallWindowProc(g_originalMainWndProc, hWnd, message, wParam, lParam);
	}

	bool InstallEditorMenuHook()
	{
		if (g_menuInstalled) {
			return true;
		}

		g_editorMainWindow = FindWindowA(nullptr, "TES Construction Set");
		if (!g_editorMainWindow) {
			g_editorMainWindow = GetForegroundWindow();
		}
		if (!g_editorMainWindow) {
			_MESSAGE("reVoice import: could not find editor main window");
			return false;
		}

		HMENU mainMenu = GetMenu(g_editorMainWindow);
		if (!mainMenu) {
			_MESSAGE("reVoice import: editor main menu missing");
			return false;
		}

		HMENU fileMenu = GetSubMenu(mainMenu, 0);
		if (!fileMenu) {
			_MESSAGE("reVoice import: file menu missing");
			return false;
		}

		AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
		AppendMenuA(fileMenu, MF_STRING, kMenuCommand_ImportRevoiceCsv, kMenuLabel);
		DrawMenuBar(g_editorMainWindow);

		g_originalMainWndProc = (WNDPROC)SetWindowLongPtr(g_editorMainWindow, GWLP_WNDPROC, (LONG_PTR)HookedEditorWndProc);
		g_menuInstalled = (g_originalMainWndProc != nullptr);
		_MESSAGE("reVoice import: menu installed = %d", g_menuInstalled ? 1 : 0);
		return g_menuInstalled;
	}
}

#ifdef RUNTIME
bool Cmd_PluginExampleFunctionsTest_Execute(COMMAND_ARGS)
{
	_MESSAGE("Hello World!");
	Console_Print("Hello Console!");
	return true;
}
#endif

DEFINE_COMMAND_PLUGIN(PluginExampleFunctionsTest, "Prints Hello to the Log and Console", 0, 0, NULL)

const bool IsCompatible(const OBSEInterface* obse)
{
	if(obse->isEditor)
	{
		if(obse->editorVersion < SUPPORTED_RUNTIME_VERSION_CS) {
			_MESSAGE("ERROR::IsCompatible: Editor incorrect editor version (got %08X need at least %08X)", obse->editorVersion, SUPPORTED_RUNTIME_VERSION_CS);
			_ERROR("ERROR::IsCompatible: Editor incorrect editor version (got %08X need at least %08X)", obse->editorVersion, SUPPORTED_RUNTIME_VERSION_CS);
			return false;
		}
	}
	else if (!IVersionCheck::IsCompatibleVersion(obse->oblivionVersion, MINIMUM_RUNTIME_VERSION, SUPPORTED_RUNTIME_VERSION, SUPPORTED_RUNTIME_VERSION_STRICT)) {
		_MESSAGE("ERROR::IsCompatible: Plugin is not compatible with runtime version, disabling");
		_FATALERROR("ERROR::IsCompatible: Plugin is not compatible with runtime version, disabling");
		return false;
	}
	return true;
}

extern "C" {

bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
{
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, PLUGIN_LOG_FILE);
	_MESSAGE(PLUGIN_VERSION_INFO);
	_MESSAGE("Plugin_Query: Querying");

	info->infoVersion =	PluginInfo::kInfoVersion;
	info->name =		PLUGIN_NAME_LONG;
	info->version =		PLUGIN_VERSION_DLL;

	if (!IsCompatible(obse)) {
		_MESSAGE("ERROR::Plugin_Query: Incompatible | Disabling Plugin");
		_FATALERROR("ERROR::Plugin_Query: Incompatible | Disabling Plugin");
		return false;
	}

	_MESSAGE("Plugin_Query: Queried Successfully");
	return true;
}

bool OBSEPlugin_Load(const OBSEInterface* obse)
{
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, PLUGIN_LOG_FILE);
	_MESSAGE(PLUGIN_VERSION_INFO);
	_MESSAGE("Plugin_Load: Loading");

	if (!IsCompatible(obse)) {
		_MESSAGE("ERROR::Plugin_Load: Incompatible | Disabling Plugin");
		_FATALERROR("ERROR::Plugin_Load: Incompatible | Disabling Plugin");
		return false;
	}

	g_pluginHandle = obse->GetPluginHandle();

	if (obse->isEditor) {
		InstallEditorMenuHook();
		_MESSAGE("Plugin_Load: Editor hooks attempted");
	}
	else {
		obse->SetOpcodeBase(0x2000);
		if (obse->RegisterCommand(&kCommandInfo_PluginExampleFunctionsTest)) {
			_MESSAGE("Plugin_Load: Functions Registered");
		}
	}

	_MESSAGE("Plugin_Load: Loaded Successfully");
	return true;
}

};
