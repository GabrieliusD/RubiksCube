#pragma once
// Letters (Docs/NPCs.md §7): people you know write while you're away, about what happened
// (a wedding, a birth, a death) or because you've been gone too long. Each letter is written by
// hand in its writer's dialog file ("letters") and dated on the outside clock, so the same
// timeline always brings the same letters and turning time back takes them back. They wait at
// the post: the trading post in the Camp, the post office later. Pure C++ (tested).
#include "Dialog.h"
#include <string>
#include <vector>

namespace npc
{
	struct Letter
	{
		std::string id;      // "<writer>:<letter>:<day>": its read flag is "read:" + id
		int from = -1;
		double day = 0.0;    // outside days
		std::string text;
	};

	// Every letter written by `days`, oldest first.
	std::vector<Letter> LettersUpTo(const DialogLibrary& library, const PlayerLog& log, double days);
	std::vector<Letter> UnreadLetters(const DialogLibrary& library, const PlayerLog& log, double days);
	// Who keeps the post in `era`, at `days` (-1: nobody: then letters wait).
	int PostKeeper(int era, double days, const PlayerLog* log);
	// The conversation topic with the post's keeper: your letters, read out one by one.
	void LetterTopics(const DialogLibrary& library, const DialogContext& context, std::deque<Topic>& out);
}
