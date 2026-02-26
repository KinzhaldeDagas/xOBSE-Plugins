// User Defines
#include "config.h"
// OBSE
#include "obse/GameAPI.h"
#include "obse/PluginAPI.h"
#include "obse_common/SafeWrite.h"
// Legacy SDK
#include "obse/CommandTable.h"
#include "obse/ParamInfos.h"
#include "obse/GameObjects.h"
#include "obse/GameOSDepend.h"
#include "obse/Script.h"
#include "obse/GameData.h"
#include "obse/GameForms.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

// Windows
#include <shlobj.h>
#include <windows.h>
#include <commdlg.h>

PluginHandle g_pluginHandle = kPluginHandle_Invalid;

namespace
{
	constexpr UINT kMenuCommand_ImportRevoiceCsv = 0x7F50;
	constexpr UINT kMenuCommand_ExportRevoiceCsv = 0x7F51;
	constexpr const char* kMenuLabel_Import = "Import reVoice CSV -> Active Plugin...";
	constexpr const char* kMenuLabel_Export = "Export reVoice CSV <- Active Plugin...";

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
		UInt32 responseNumber = 0;
	};

	struct ImportSummary
	{
		UInt32 parsedRows = 0;
		UInt32 applied = 0;
		UInt32 skipped = 0;
		UInt32 warnings = 0;
		UInt32 errors = 0;
		UInt32 dialogueMismatch = 0;
		UInt32 pathMismatch = 0;
		std::vector<std::string> diagnostics;
	};

	struct SpeakerContext
	{
		bool concrete = false;
		std::string voiceID = "ob_unknown";
		std::string speakerInfo = "Unknown\\M";
		std::string outputFolder = "Unknown\\M";
	};

	struct EditorResponseData
	{
		UInt32 unk00;
		UInt32 unk04;
		UInt32 unk08;
		UInt32 unk0C;
		BSStringT responseText;
		UInt32 responseNumber;
	};

	struct EditorTopicInfo
	{
		TESForm base;
		TESTopic* unk24;
		ConditionEntry conditions;
		UInt16 unk30;
		UInt16 infotype;
		UInt8 flags0;
		UInt8 pad35[3];
		tList<TESTopic> addedTopics;
		void* linkedTopics;
		tList<EditorResponseData> responseList;
		Script resultScript;
	};

	std::string Trim(const std::string& in)
	{
		size_t start = 0;
		while (start < in.size() && std::isspace((unsigned char)in[start])) {
			++start;
		}
		size_t end = in.size();
		while (end > start && std::isspace((unsigned char)in[end - 1])) {
			--end;
		}
		return in.substr(start, end - start);
	}

	std::string SanitizePathComponent(const std::string& in)
	{
		std::string out;
		out.reserve(in.size());
		for (char ch : in)
		{
			if (std::isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == ' ') {
				out.push_back(ch);
			}
			else {
				out.push_back('_');
			}
		}
		out = Trim(out);
		return out.empty() ? "Unknown" : out;
	}

	std::vector<std::string> ParseDelimitedLine(const std::string& line, char delimiter)
	{
		std::vector<std::string> out;
		std::string current;
		bool inQuotes = false;
		for (size_t i = 0; i < line.size(); ++i)
		{
			char ch = line[i];
			if (ch == '"')
			{
				if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
					current.push_back('"');
					++i;
				}
				else {
					inQuotes = !inQuotes;
				}
			}
			else if (ch == delimiter && !inQuotes)
			{
				out.push_back(current);
				current.clear();
			}
			else {
				current.push_back(ch);
			}
		}
		out.push_back(current);
		return out;
	}

	bool ParseHexFormID(const std::string& text, UInt32& outFormID)
	{
		std::string clean = Trim(text);
		if (clean.size() != 8) {
			return false;
		}
		for (char ch : clean) {
			if (!std::isxdigit((unsigned char)ch)) {
				return false;
			}
		}
		outFormID = strtoul(clean.c_str(), nullptr, 16);
		return true;
	}

	std::string NormalizeVoiceOutputPath(const std::string& raw)
	{
		std::string path = Trim(raw);
		std::replace(path.begin(), path.end(), '/', '\\');
		while (!path.empty() && (path.front() == '\\' || path.front() == '/')) {
			path.erase(path.begin());
		}
		if (path.size() > 1 && std::isalpha((unsigned char)path[0]) && path[1] == ':') {
			path = path.substr(2);
		}
		while (!path.empty() && (path.front() == '\\' || path.front() == '/')) {
			path.erase(path.begin());
		}
		if (_strnicmp(path.c_str(), "Data\\", 5) == 0) {
			path = path.substr(5);
		}

		std::string collapsed;
		collapsed.reserve(path.size());
		bool prevSlash = false;
		for (char ch : path)
		{
			const bool slash = (ch == '\\');
			if (slash && prevSlash) {
				continue;
			}
			collapsed.push_back(ch);
			prevSlash = slash;
		}

		if (_strnicmp(collapsed.c_str(), "Sound\\Voice\\", 12) != 0) {
			return "";
		}
		if (collapsed.find("..") != std::string::npos) {
			return "";
		}
		return collapsed;
	}

	UInt32 ParseResponseNumberFromPath(const std::string& outputPath)
	{
		auto dot = outputPath.find_last_of('.');
		std::string base = dot == std::string::npos ? outputPath : outputPath.substr(0, dot);
		auto under = base.find_last_of('_');
		if (under == std::string::npos) {
			return 0;
		}
		std::string tail = base.substr(under + 1);
		if (tail.empty()) {
			return 0;
		}
		for (char ch : tail) {
			if (!std::isdigit((unsigned char)ch)) {
				return 0;
			}
		}
		return static_cast<UInt32>(atoi(tail.c_str()));
	}

	bool IsEditorLoaded()
	{
		return g_editorDataHandler && *g_editorDataHandler;
	}

	DataHandler* GetEditorDataHandler()
	{
		return IsEditorLoaded() ? *g_editorDataHandler : nullptr;
	}

	ModEntry::Data* GetActivePlugin()
	{
		DataHandler* handler = GetEditorDataHandler();
		if (!handler) {
			return nullptr;
		}
		return handler->unk8B8.activeFile;
	}

	SpeakerContext BuildSpeakerContext(EditorTopicInfo* info)
	{
		SpeakerContext ctx;
		if (!info) {
			return ctx;
		}

		TESForm* raceForm = nullptr;
		TESForm* idForm = nullptr;
		int sex = -1;

		for (ConditionEntry* node = &info->conditions; node; node = node->next)
		{
			if (!node->data) {
				continue;
			}
			const UInt16 fn = node->data->functionIndex & 0x0FFF;
			if (fn == 224 && node->data->param1.form) {
				raceForm = node->data->param1.form;
			}
			else if (fn == 72 && node->data->param1.form) {
				idForm = node->data->param1.form;
			}
			else if (fn == 69) {
				const int sexVal = static_cast<int>(node->data->comparisonValue);
				if (sexVal == 0 || sexVal == 1) {
					sex = sexVal;
				}
			}
		}

		if (raceForm && raceForm->typeID == kFormType_Race)
		{
			ctx.concrete = true;
			const std::string raceLabel = SanitizePathComponent(raceForm->GetEditorID() ? raceForm->GetEditorID() : "Unknown");
			const char* sexLabel = (sex == 1) ? "F" : "M";
			ctx.speakerInfo = raceLabel + "\\" + sexLabel;
			ctx.outputFolder = ctx.speakerInfo;
		}

		if (idForm && (idForm->typeID == kFormType_NPC || idForm->typeID == kFormType_Creature))
		{
			ctx.concrete = true;
			const std::string idLabel = SanitizePathComponent(idForm->GetEditorID() ? idForm->GetEditorID() : "Unknown");
			const char* sexLabel = (sex == 1) ? "F" : "M";
			ctx.speakerInfo = std::string("NPC\\") + idLabel + "\\" + sexLabel;
			if (ctx.outputFolder == "Unknown\\M") {
				ctx.outputFolder = ctx.speakerInfo;
			}
		}

		return ctx;
	}

	bool FindParentTopicAndQuest(DataHandler* handler, EditorTopicInfo* target, TESTopic*& outTopic, TESQuest*& outQuest)
	{
		outTopic = nullptr;
		outQuest = nullptr;
		if (!handler || !target) {
			return false;
		}

		for (tList<TESTopic>::Iterator topicIt = handler->topics.Begin(); !topicIt.End() && topicIt.Get(); ++topicIt)
		{
			TESTopic* topic = topicIt.Get();
			if (!topic) {
				continue;
			}
			for (TESTopic::QuestInfoEntry* qEntry = topic->questInfoList; qEntry; qEntry = qEntry->next)
			{
				if (!qEntry->data) {
					continue;
				}
				for (UInt32 i = 0; i < qEntry->data->infoList.numObjs; ++i)
				{
					TESTopicInfo* info = qEntry->data->infoList.data[i];
					if ((void*)info == (void*)target) {
						outTopic = topic;
						outQuest = qEntry->data->parentQuest;
						return true;
					}
				}
			}
		}

		return false;
	}

	std::string BuildRevoiceOutputPath(EditorTopicInfo* info, TESTopic* topic, TESQuest* quest, EditorResponseData* response, const SpeakerContext& ctx)
	{
		ModEntry::Data* activeFile = GetActivePlugin();
		if (!info || !topic || !quest || !response || !activeFile || !ctx.concrete) {
			return "";
		}

		const char* questIDRaw = quest->GetEditorID();
		const char* topicIDRaw = topic->GetEditorID();
		if (!questIDRaw || !topicIDRaw) {
			return "";
		}
		const std::string questID = SanitizePathComponent(questIDRaw);
		const std::string topicID = SanitizePathComponent(topicIDRaw);

		char buffer[MAX_PATH * 2] = {0};
		snprintf(buffer, sizeof(buffer),
			"Sound\\Voice\\%s\\%s\\%s_%s_%08X_%u.mp3",
			activeFile->name,
			ctx.outputFolder.c_str(),
			questID.c_str(),
			topicID.c_str(),
			(info->base.refID & 0xFFFFFF),
			response->responseNumber);

		return NormalizeVoiceOutputPath(buffer);
	}

	std::vector<RevoiceRow> ParseRevoiceFile(const std::string& filePath, ImportSummary& summary)
	{
		std::vector<RevoiceRow> rows;
		std::ifstream input(filePath, std::ios::binary);
		if (!input.good()) {
			summary.errors++;
			summary.diagnostics.push_back("Couldn't open selected file.");
			return rows;
		}

		std::string line;
		UInt32 lineNo = 0;
		while (std::getline(input, line))
		{
			++lineNo;
			if (lineNo == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
				line = line.substr(3);
			}
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (Trim(line).empty()) {
				continue;
			}

			auto fields = ParseDelimitedLine(line, '\t');
			if (fields.size() < 5) {
				fields = ParseDelimitedLine(line, ',');
			}
			if (fields.size() < 5) {
				summary.errors++;
				summary.diagnostics.push_back("Line " + std::to_string(lineNo) + ": malformed row.");
				continue;
			}
			if (lineNo == 1 && _stricmp(Trim(fields[0]).c_str(), "FormID") == 0) {
				continue;
			}

			RevoiceRow row;
			row.lineNumber = lineNo;
			if (!ParseHexFormID(fields[0], row.formID)) {
				summary.errors++;
				summary.diagnostics.push_back("Line " + std::to_string(lineNo) + ": invalid FormID.");
				continue;
			}
			row.voiceID = Trim(fields[1]);
			row.speakerInfo = Trim(fields[2]);
			row.outputPath = NormalizeVoiceOutputPath(fields[3]);
			row.dialogue = Trim(fields[4]);
			row.responseNumber = ParseResponseNumberFromPath(row.outputPath);

			if (row.outputPath.empty() || row.dialogue.empty() || row.responseNumber == 0) {
				summary.errors++;
				summary.diagnostics.push_back("Line " + std::to_string(lineNo) + ": invalid path/dialogue/response number.");
				continue;
			}

			rows.push_back(std::move(row));
		}
		return rows;
	}

	void ShowImportSummary(const ImportSummary& summary)
	{
		std::ostringstream ss;
		ss << "reVoice import complete.\n\n"
			<< "Parsed rows: " << summary.parsedRows << "\n"
			<< "Updated responses: " << summary.applied << "\n"
			<< "Skipped rows: " << summary.skipped << "\n"
			<< "Warnings (dialogue mismatch): " << summary.dialogueMismatch << "\n"
			<< "Warnings (output path mismatch): " << summary.pathMismatch << "\n"
			<< "Warnings (other): " << summary.warnings << "\n"
			<< "Parse/validation errors: " << summary.errors;
		MessageBoxA(g_editorMainWindow, ss.str().c_str(), "Import reVoice CSV -> Active Plugin", MB_OK | MB_ICONINFORMATION);
	}

	void ImportRevoiceCsvToActivePlugin()
	{
		DataHandler* handler = GetEditorDataHandler();
		ModEntry::Data* activeFile = GetActivePlugin();
		if (!handler || !activeFile) {
			MessageBoxA(g_editorMainWindow, "An active plugin must be set before using this tool.", "Import reVoice CSV", MB_OK | MB_ICONERROR);
			return;
		}

		char filePath[MAX_PATH] = {0};
		OPENFILENAMEA ofn = {0};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = g_editorMainWindow;
		ofn.lpstrFilter = "reVoice CSV/TSV\0*.csv;*.tsv;*.txt\0All Files\0*.*\0\0";
		ofn.lpstrFile = filePath;
		ofn.nMaxFile = sizeof(filePath);
		ofn.lpstrTitle = "Select reVoice export CSV/TSV";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
		if (!GetOpenFileNameA(&ofn)) {
			return;
		}

		ImportSummary summary;
		std::vector<RevoiceRow> rows = ParseRevoiceFile(filePath, summary);
		summary.parsedRows = static_cast<UInt32>(rows.size());

		std::unordered_map<UInt32, size_t> lastRowForForm;
		for (size_t i = 0; i < rows.size(); ++i) {
			lastRowForForm[rows[i].formID] = i;
		}

		for (size_t i = 0; i < rows.size(); ++i)
		{
			const RevoiceRow& row = rows[i];
			if (lastRowForForm[row.formID] != i) {
				summary.warnings++;
				continue;
			}

			TESForm* form = EditorLookupFormByID ? EditorLookupFormByID(row.formID) : nullptr;
			if (!form || form->typeID != kFormType_DialogInfo) {
				summary.skipped++;
				continue;
			}
			if ((form->flags & TESForm::kFormFlags_FromActiveFile) == 0) {
				summary.skipped++;
				continue;
			}

			EditorTopicInfo* info = reinterpret_cast<EditorTopicInfo*>(form);
			const SpeakerContext ctx = BuildSpeakerContext(info);
			if (!ctx.concrete) {
				summary.skipped++;
				continue;
			}

			EditorResponseData* targetResponse = nullptr;
			for (tList<EditorResponseData>::Iterator rIt = info->responseList.Begin(); !rIt.End() && rIt.Get(); ++rIt)
			{
				EditorResponseData* response = rIt.Get();
				if (response && response->responseNumber == row.responseNumber) {
					targetResponse = response;
					break;
				}
			}
			if (!targetResponse) {
				summary.skipped++;
				continue;
			}

			TESTopic* parentTopic = nullptr;
			TESQuest* parentQuest = nullptr;
			FindParentTopicAndQuest(handler, info, parentTopic, parentQuest);
			const std::string expectedPath = BuildRevoiceOutputPath(info, parentTopic, parentQuest, targetResponse, ctx);
			if (!expectedPath.empty() && _stricmp(expectedPath.c_str(), row.outputPath.c_str()) != 0) {
				summary.pathMismatch++;
			}

			const char* existing = targetResponse->responseText.m_data ? targetResponse->responseText.m_data : "";
			if (_stricmp(Trim(existing).c_str(), row.dialogue.c_str()) != 0) {
				summary.dialogueMismatch++;
			}

			targetResponse->responseText.Set(row.dialogue.c_str());
			form->SetFromActiveFile(true);
			summary.applied++;
		}

		ShowImportSummary(summary);
	}

	std::string EscapeDialogueForTsv(const std::string& in)
	{
		std::string out = in;
		for (char& ch : out) {
			if (ch == '\t' || ch == '\r' || ch == '\n') {
				ch = ' ';
			}
		}
		return out;
	}

	void ExportRevoiceCsvForActivePlugin()
	{
		DataHandler* handler = GetEditorDataHandler();
		ModEntry::Data* activeFile = GetActivePlugin();
		if (!handler || !activeFile) {
			MessageBoxA(g_editorMainWindow, "An active plugin must be set before using this tool.", "Export reVoice CSV", MB_OK | MB_ICONERROR);
			return;
		}

		char filePath[MAX_PATH] = {0};
		snprintf(filePath, sizeof(filePath), "%s_revoice.csv", activeFile->name);

		OPENFILENAMEA ofn = {0};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = g_editorMainWindow;
		ofn.lpstrFilter = "CSV Files\0*.csv\0All Files\0*.*\0\0";
		ofn.lpstrFile = filePath;
		ofn.nMaxFile = sizeof(filePath);
		ofn.lpstrTitle = "Export reVoice CSV for active plugin";
		ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
		if (!GetSaveFileNameA(&ofn)) {
			return;
		}

		std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
		if (!out.good()) {
			MessageBoxA(g_editorMainWindow, "Couldn't create output CSV file.", "Export reVoice CSV", MB_OK | MB_ICONERROR);
			return;
		}

		out << "FormID\tVoiceID\tSpeakerInfo\tOutputPath\tDialogue\n";
		int exported = 0;
		int skipped = 0;

		for (tList<TESTopic>::Iterator topicIt = handler->topics.Begin(); !topicIt.End() && topicIt.Get(); ++topicIt)
		{
			TESTopic* topic = topicIt.Get();
			if (!topic) {
				continue;
			}

			for (TESTopic::QuestInfoEntry* qEntry = topic->questInfoList; qEntry; qEntry = qEntry->next)
			{
				if (!qEntry->data || !qEntry->data->parentQuest) {
					continue;
				}

				for (UInt32 i = 0; i < qEntry->data->infoList.numObjs; ++i)
				{
					TESTopicInfo* baseInfo = qEntry->data->infoList.data[i];
					if (!baseInfo) {
						continue;
					}
					TESForm* baseForm = reinterpret_cast<TESForm*>(baseInfo);
					if ((baseForm->flags & TESForm::kFormFlags_FromActiveFile) == 0) {
						continue;
					}

					EditorTopicInfo* info = reinterpret_cast<EditorTopicInfo*>(baseForm);
					const SpeakerContext ctx = BuildSpeakerContext(info);
					if (!ctx.concrete) {
						++skipped;
						continue;
					}

					for (tList<EditorResponseData>::Iterator rIt = info->responseList.Begin(); !rIt.End() && rIt.Get(); ++rIt)
					{
						EditorResponseData* response = rIt.Get();
						if (!response) {
							continue;
						}

						const std::string outPath = BuildRevoiceOutputPath(info, topic, qEntry->data->parentQuest, response, ctx);
						if (outPath.empty()) {
							++skipped;
							continue;
						}

						char formIDBuffer[16] = {0};
						snprintf(formIDBuffer, sizeof(formIDBuffer), "%08X", baseForm->refID);
						const char* text = response->responseText.m_data ? response->responseText.m_data : "";
						out << formIDBuffer << '\t'
							<< ctx.voiceID << '\t'
							<< ctx.speakerInfo << '\t'
							<< outPath << '\t'
							<< EscapeDialogueForTsv(text) << '\n';
						++exported;
					}
				}
			}
		}

		std::ostringstream ss;
		ss << "reVoice export complete.\n\nExported rows: " << exported << "\nSkipped rows: " << skipped << "\nOutput: " << filePath;
		MessageBoxA(g_editorMainWindow, ss.str().c_str(), "Export reVoice CSV <- Active Plugin", MB_OK | MB_ICONINFORMATION);
	}

	LRESULT CALLBACK HookedEditorWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_COMMAND)
		{
			switch (LOWORD(wParam))
			{
			case kMenuCommand_ImportRevoiceCsv:
				ImportRevoiceCsvToActivePlugin();
				return 0;
			case kMenuCommand_ExportRevoiceCsv:
				ExportRevoiceCsvForActivePlugin();
				return 0;
			default:
				break;
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
			_MESSAGE("reVoice: could not locate editor main window");
			return false;
		}

		HMENU mainMenu = GetMenu(g_editorMainWindow);
		if (!mainMenu) {
			_MESSAGE("reVoice: editor menu not found");
			return false;
		}

		HMENU fileMenu = GetSubMenu(mainMenu, 0);
		if (!fileMenu) {
			_MESSAGE("reVoice: file menu not found");
			return false;
		}

		AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
		AppendMenuA(fileMenu, MF_STRING, kMenuCommand_ImportRevoiceCsv, kMenuLabel_Import);
		AppendMenuA(fileMenu, MF_STRING, kMenuCommand_ExportRevoiceCsv, kMenuLabel_Export);
		DrawMenuBar(g_editorMainWindow);

		g_originalMainWndProc = (WNDPROC)SetWindowLongPtr(g_editorMainWindow, GWLP_WNDPROC, (LONG_PTR)HookedEditorWndProc);
		g_menuInstalled = (g_originalMainWndProc != nullptr);
		_MESSAGE("reVoice: menu installed=%d", g_menuInstalled ? 1 : 0);
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
	if (obse->isEditor)
	{
		if (obse->editorVersion < SUPPORTED_RUNTIME_VERSION_CS) {
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

	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = PLUGIN_NAME_LONG;
	info->version = PLUGIN_VERSION_DLL;

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
