#include "NpcTools.h"
#include "Life.h"
#include "Clock.h"
#include "Config.h"
#include "SceneComponents.h"
#include <Core/Log.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace DirectX;

namespace
{
	const ImVec4 kMatch(0.55f, 0.9f, 0.55f, 1.0f);
	const ImVec4 kChosen(1.0f, 0.8f, 0.45f, 1.0f);
	const ImVec4 kDim(0.6f, 0.6f, 0.6f, 1.0f);
	const char* const kEras[] = { "the year's", "Camp", "Settlement", "Boomtown", "Industry", "Abandoned", "Gone" };
	const char* const kTiers[] = { "Hostile", "Wary", "Stranger", "Acquaintance", "Friendly", "Friend", "Close" };

	std::string Clock(double days)
	{
		OutsideClock c;
		c.SetDays(days);
		return c.DateText();
	}

	std::string HourText(double days)
	{
		const double hour = (days - std::floor(days)) * 24.0;
		char text[16];
		std::snprintf(text, sizeof(text), "%02d:%02d", static_cast<int>(hour), static_cast<int>(std::fmod(hour, 1.0) * 60.0));
		return text;
	}

	std::string EventText(const npc::TownEvent& e)
	{
		using K = npc::TownEventKind;
		const std::vector<npc::NpcDef>& cast = npc::Cast();
		switch (e.kind)
		{
		case K::Funeral: return std::string("Funeral of ") + cast[e.npc].name + " (" + HourText(e.day) + "-" + HourText(e.until) + ", " + e.gather + ")";
		case K::Festival: return "Founders' Day (" + HourText(e.day) + "-" + HourText(e.until) + ", " + e.gather + ")";
		case K::Wedding: return npc::NewsLine(e) + " (" + HourText(e.day) + "-" + HourText(e.until) + ", " + e.gather + ")";
		default: return npc::NewsLine(e);
		}
	}

	// Who and when, picked with a combo.
	bool PickPerson(const char* label, int& who)
	{
		const std::vector<npc::NpcDef>& cast = npc::Cast();
		who = std::clamp(who, 0, static_cast<int>(cast.size()) - 1);
		bool changed = false;
		if (ImGui::BeginCombo(label, cast[who].name))
		{
			for (int i = 0; i < static_cast<int>(cast.size()); ++i)
				if (ImGui::Selectable(cast[i].name, i == who)) { who = i; changed = true; }
			ImGui::EndCombo();
		}
		return changed;
	}

	void Wrapped(const ImVec4& colour, const std::string& text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, colour);
		ImGui::TextWrapped("%s", text.c_str());
		ImGui::PopStyleColor();
	}
}

// =============================================================================
// People inspector
// =============================================================================

void NpcTools::DrawInspector(const Data& data, DebugDraw* debug, Request& request)
{
	if (!data.town || !data.town->Built())
	{
		ImGui::TextDisabled("The town isn't built (the scene needs a ValleyWorld and the valley's props).");
		return;
	}
	PickPerson("Person", mWho);
	if (data.playing)
	{
		ImGui::SameLine();
		ImGui::Checkbox("Now", &mFollowClock);
		if (mFollowClock && data.now >= 0.0)
		{
			mYears = static_cast<float>(data.now / 365.0);
			mHour = static_cast<float>((data.now - std::floor(data.now)) * 24.0);
		}
	}
	bool timeChanged = ImGui::SliderFloat("Years since arrival", &mYears, 0.0f, 110.0f, "%.2f");
	timeChanged |= ImGui::SliderFloat("Hour", &mHour, 0.0f, 23.99f, "%.1f");
	if (timeChanged) mFollowClock = false;
	ImGui::Combo("Era", &mEraOverride, kEras, IM_ARRAYSIZE(kEras));
	ImGui::SameLine();
	ImGui::TextDisabled("(the town shown)");
	const double days = std::floor(mYears * 365.0) + mHour / 24.0;
	const int era = mEraOverride > 0 ? mEraOverride : OutsideClock::EraAt(days / 365.0);
	ImGui::Text("%s  -  %s", Clock(days).c_str(), kEras[era]);
	if (!data.playing) ImGui::Checkbox("Show in the viewport (their spot, today's places and walks)", &mDraw);
	else
	{
		if (ImGui::SmallButton("Set the clock to this time"))
		{
			request.setClock = true;
			request.days = days;
		}
		const npc::Pose pose = data.town->Evaluate(mWho, days, era, data.log);
		if (pose.present)
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Go to them (sets the clock too)"))
			{
				request.setClock = true;
				request.days = days;
				request.goTo = true;
				request.feet = { pose.position.x + std::sin(pose.yaw) * 2.5f, pose.position.y + 0.5f, pose.position.z + std::cos(pose.yaw) * 2.5f };
				request.yaw = pose.yaw + XM_PI;
			}
		}
		ImGui::SameLine();
		ImGui::Checkbox("Show in the world", &mDraw);
	}
	ImGui::Separator();
	DrawLife(data, mWho, days, era);
	DrawDay(data, mWho, days, era, mDraw ? debug : nullptr);
}

void NpcTools::DrawLife(const Data& data, int who, double days, int era)
{
	const std::vector<npc::NpcDef>& cast = npc::Cast();
	const npc::NpcDef& n = cast[who];
	const npc::LifeState life = npc::LifeAt(who, days, data.log);
	const npc::Pose pose = data.town->Evaluate(who, days, era, data.log);

	ImGui::TextColored(kChosen, "%s", n.name);
	ImGui::SameLine();
	ImGui::TextDisabled("%s, %s voice%s", life.role, n.voice, n.ageless ? ", ageless" : "");
	const char* status = !life.born ? "not born yet" : !life.alive ? "dead" : days < n.arrives * 365.0 ? "not in the valley yet"
		: days >= life.leavesDay ? "left the valley" : npc::DayFor(n, era).empty() ? "alive, but no day in this era's town" : "living in the valley";
	if (n.ageless) ImGui::Text("Status: %s, ageless", status);
	else ImGui::Text("Status: %s, age %.0f", status, life.age);
	if (pose.present)
	{
		if (pose.activity == npc::Activity::Walk)
			ImGui::Text("Now: walking from %s to %s%s", data.town->PlaceName(pose.from).c_str(), data.town->PlaceName(pose.place).c_str(), pose.gathering ? " (a gathering)" : "");
		else ImGui::Text("Now: %s at %s%s%s", npc::ActivityName(pose.activity), data.town->PlaceName(pose.place).c_str(), pose.rain ? " (rain)" : "", pose.gathering ? " (a gathering)" : "");
	}
	if (data.log && life.born)
	{
		const npc::Standing s = data.log->StandingOf(who, days);
		ImGui::Text("Thinks of you: %s (%+.0f)%s", npc::TierName(s.tier), s.regard, s.met ? "" : ", never met");
	}

	if (ImGui::TreeNode("Story, traits, likes, family"))
	{
		Wrapped(kDim, n.story);
		const float traits[] = { n.traits.warmth, n.traits.greed, n.traits.patience, n.traits.superstition, n.traits.curiosity };
		const char* const names[] = { "warmth", "greed", "patience", "superstition", "curiosity" };
		for (int i = 0; i < 5; ++i)
		{
			ImGui::ProgressBar(traits[i], ImVec2(140.0f, 0.0f), "");
			ImGui::SameLine();
			ImGui::Text("%s %.2f", names[i], traits[i]);
		}
		std::string likes, dislikes, ties, children;
		for (const char* l : n.likes) likes += (likes.empty() ? "" : ", ") + std::string(l);
		for (const char* d : n.dislikes) dislikes += (dislikes.empty() ? "" : ", ") + std::string(d);
		for (const npc::Tie& t : n.ties) ties += (ties.empty() ? "" : ", ") + std::string(t.relation) + ": " + t.id;
		for (const npc::NpcDef& other : cast)
			for (const char* p : other.parents) if (std::string(p) == n.id) children += (children.empty() ? "" : ", ") + std::string(other.name);
		ImGui::Text("Likes: %s   Dislikes: %s", likes.empty() ? "-" : likes.c_str(), dislikes.empty() ? "-" : dislikes.c_str());
		ImGui::TextWrapped("Ties: %s", ties.empty() ? "-" : ties.c_str());
		if (n.spouse) ImGui::Text("Married to %s in year %.1f", n.spouse, n.marries);
		if (!children.empty()) ImGui::Text("Children: %s", children.c_str());
		if (n.epitaph) ImGui::Text("Epitaph: \"%s\"", n.epitaph);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Life, year by year"))
	{
		for (const npc::TownEvent& e : npc::EventsBetween(-1.0, 120.0 * 365.0, data.log))
		{
			if (e.kind == npc::TownEventKind::Festival) continue;
			const bool theirs = e.npc == who || e.other == who;
			const bool attends = e.Gathering() && npc::Attends(e, who, data.log);
			if (!theirs && !attends) continue;
			const bool past = e.day <= days;
			ImGui::TextColored(past ? kMatch : kDim, "year %5.2f  %s%s", e.day / 365.0, EventText(e).c_str(), !theirs && attends ? "  (goes)" : "");
		}
		int festivals = 0;
		for (const npc::TownEvent& e : npc::EventsBetween(-1.0, 120.0 * 365.0, data.log))
			festivals += e.kind == npc::TownEventKind::Festival && npc::Attends(e, who, data.log) ? 1 : 0;
		ImGui::TextDisabled("and %d Founders' Days", festivals);
		for (const npc::LifeHook& h : n.hooks)
			ImGui::TextDisabled("if flag '%s' is set in time: %s", h.flag, h.dies >= 0.0 ? ("dies at year " + std::to_string(static_cast<int>(h.dies))).c_str() : "leaves later");
		ImGui::TreePop();
	}

	if (data.missions && ImGui::TreeNode("Missions they give"))
	{
		int count = 0;
		for (const npc::MissionDef& m : data.missions->Written())
		{
			if (m.giver != n.id) continue;
			++count;
			ImGui::Text("%s", m.title.c_str());
			const std::string when = npc::Describe(m.when);
			ImGui::SameLine();
			ImGui::TextDisabled("(%s; %s)", m.id.c_str(), when.empty() ? "always offered" : when.c_str());
			if (data.log)
			{
				const npc::MissionStatus s = data.missions->Status(*data.log, m.id, days);
				static const char* const kStates[] = { "open", "running", "done", "failed" };
				ImGui::SameLine();
				ImGui::TextColored(kChosen, "%s", kStates[static_cast<int>(s.state)]);
			}
		}
		if (count == 0) ImGui::TextDisabled("None.");
		ImGui::TreePop();
	}
	if (data.dialogs && ImGui::TreeNode("Letters they write"))
	{
		const npc::DialogFile* file = data.dialogs->For(who);
		if (!file || file->letters.empty()) ImGui::TextDisabled("None written.");
		else
			for (const npc::LetterDef& l : file->letters)
				ImGui::BulletText("on %s%s%s: %.60s...", l.on.c_str(), l.about.empty() ? "" : (" about " + l.about).c_str(),
					l.on == "away" ? (" (" + std::to_string(static_cast<int>(l.days)) + " days)").c_str() : "", l.say.front().c_str());
		if (data.log)
			for (const npc::Letter& l : npc::LettersUpTo(*data.dialogs, *data.log, days))
				if (l.from == who) ImGui::TextColored(kMatch, "sent %s: %.60s...", Clock(l.day).c_str(), l.text.c_str());
		ImGui::TreePop();
	}
}

void NpcTools::DrawDay(const Data& data, int who, double days, int era, DebugDraw* debug)
{
	const npc::Town& town = *data.town;
	const long long day = static_cast<long long>(std::floor(days));
	const std::vector<npc::Town::Block> blocks = town.DayBlocks(who, day, era, data.log);
	if (!ImGui::TreeNodeEx("Their day", ImGuiTreeNodeFlags_DefaultOpen)) return;
	if (blocks.empty()) ImGui::TextDisabled("No day in this era's town.");
	else if (ImGui::BeginTable("day", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
	{
		for (size_t i = 0; i < blocks.size(); ++i)
		{
			const npc::Town::Block& b = blocks[i];
			const bool now = days >= b.start && (i + 1 == blocks.size() || days < blocks[i + 1].start);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextColored(now ? kChosen : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", HourText(b.start).c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%s", town.PlaceName(b.place).c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%s", npc::ActivityName(b.activity));
			ImGui::TableNextColumn();
			if (b.gathering) ImGui::TextColored(kMatch, "gathering");
			else if (b.rain) ImGui::TextDisabled("rain place");
		}
		ImGui::EndTable();
	}
	ImGui::TreePop();

	// In the viewport: today's places joined by their walks, and where they are now.
	if (!debug || blocks.empty()) return;
	const uint32_t walk = DebugDraw::Color(1.0f, 0.75f, 0.2f), spot = DebugDraw::Color(0.3f, 0.95f, 1.0f), here = DebugDraw::Color(1.0f, 0.3f, 0.9f);
	auto y = [&](const npc::RoutePoint& p) { return (p.ground ? town.GroundY(p.x, p.z) : p.y) + 0.3f; };
	for (size_t i = 0; i < blocks.size(); ++i)
	{
		if (blocks[i].place < 0 || !town.PlaceAt(blocks[i].place).resolved) continue;
		const npc::RoutePoint& at = town.PlaceAt(blocks[i].place).at;
		debug->Line({ at.x, y(at), at.z }, { at.x, y(at) + 2.0f, at.z }, spot);
		const int previous = i > 0 ? blocks[i - 1].place : blocks.back().place;
		if (previous < 0 || previous == blocks[i].place || !town.PlaceAt(previous).resolved) continue;
		const npc::Town::Route& route = town.RouteBetween(previous, blocks[i].place);
		for (size_t k = 1; k < route.points.size(); ++k)
			debug->Line({ route.points[k - 1].x, y(route.points[k - 1]), route.points[k - 1].z }, { route.points[k].x, y(route.points[k]), route.points[k].z }, walk);
	}
	const npc::Pose pose = town.Evaluate(who, days, era, data.log);
	if (pose.present)
	{
		const XMFLOAT3 p = pose.position;
		debug->Line({ p.x, p.y, p.z }, { p.x, p.y + 4.0f, p.z }, here);
		debug->Line({ p.x - 0.6f, p.y + 0.1f, p.z }, { p.x + 0.6f, p.y + 0.1f, p.z }, here);
		debug->Line({ p.x, p.y + 0.1f, p.z - 0.6f }, { p.x, p.y + 0.1f, p.z + 0.6f }, here);
	}
}

// =============================================================================
// Dialog view
// =============================================================================

npc::DialogContext NpcTools::SandboxContext() const
{
	npc::DialogContext c;
	c.npc = mTalker;
	c.days = std::floor(mSandboxYears * 365.0) + mSandboxHour / 24.0;
	c.hour = mSandboxHour;
	c.rain = mRain;
	c.night = mSandboxHour < 5.5f || mSandboxHour > 20.5f;
	c.era = OutsideClock::EraAt(c.days / 365.0);
	c.log = &mSandboxLog;
	if (mHasEverything) c.count = [](const std::string&) { return 999; };
	// The friendship as set, rather than worked out.
	static const float kRegard[] = { -70.0f, -30.0f, 0.0f, 10.0f, 32.0f, 60.0f, 90.0f };
	c.standing.met = mMet && mTier != static_cast<int>(npc::Tier::Stranger);
	c.standing.regard = kRegard[mTier];
	c.standing.tier = static_cast<npc::Tier>(mTier);
	c.standing.lastSeen = c.standing.met ? c.days - mAway : -1e30;
	c.standing.lostRegard = mDejaVu ? 40.0f : 0.0f;
	return c;
}

void NpcTools::ResetSandbox()
{
	// A log that gives the same situation (for trying a conversation and for flags).
	const npc::DialogContext c = SandboxContext();
	mSandboxLog.Clear();
	std::istringstream words(mFlags);
	for (std::string flag; words >> flag;) mSandboxLog.Add({ 0.0, -1, npc::EventKind::Flag, 0.0f, flag });
	if (c.standing.met)
	{
		mSandboxLog.Add({ c.days - mAway, mTalker, npc::EventKind::Talk, 0.0f, "" });
		const float now = mSandboxLog.StandingOf(mTalker, c.days).regard;
		mSandboxLog.Add({ c.days - 0.001, mTalker, npc::EventKind::Regard, c.standing.regard - now, "sandbox" });
	}
	else if (c.standing.regard != 0.0f) mSandboxLog.Add({ c.days - 0.001, mTalker, npc::EventKind::Regard, c.standing.regard, "sandbox" });
	mTalking = false;
}

void NpcTools::DrawLines(const char* label, const std::vector<npc::Line>& lines, const npc::DialogContext& context, bool pickOne)
{
	if (!ImGui::TreeNode(label, "%s (%zu)", label, lines.size())) return;
	int best = -1000000;
	if (pickOne)
		for (const npc::Line& l : lines) if (npc::Matches(l.when, context)) best = std::max(best, l.priority);
	for (const npc::Line& l : lines)
	{
		const bool match = npc::Matches(l.when, context);
		const std::string when = npc::Describe(l.when);
		ImGui::TextColored(match ? (pickOne && l.priority == best ? kChosen : kMatch) : kDim, "%s  [%s%s]", match ? (pickOne && l.priority == best ? ">" : "+") : " ",
			when.empty() ? "always" : when.c_str(), l.priority ? (", priority " + std::to_string(l.priority)).c_str() : "");
		ImGui::Indent();
		for (const std::string& say : l.say) Wrapped(match ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kDim, "\"" + npc::FillText(say, context) + "\"");
		ImGui::Unindent();
	}
	ImGui::TreePop();
}

void NpcTools::DrawSteps(const std::vector<npc::Step>& steps, const npc::DialogContext& context, int depth)
{
	for (const npc::Step& s : steps)
	{
		for (const npc::Effect& e : s.effects)
		{
			static const char* const kKinds[] = { "friendship", "set flag", "clear flag", "end", "accept", "complete", "take", "shop" };
			ImGui::TextColored(kMatch, "  do: %s %s%s", kKinds[static_cast<int>(e.kind)], e.name.c_str(),
				e.kind == npc::Effect::Kind::Regard || e.kind == npc::Effect::Kind::Take ? (" " + std::to_string(static_cast<int>(e.amount))).c_str() : "");
		}
		if (!s.say.empty()) Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "\"" + npc::FillText(s.say, context) + "\"");
		for (const npc::Choice& c : s.choices)
		{
			const std::string when = npc::Describe(c.when);
			const bool match = npc::Matches(c.when, context);
			ImGui::PushID(&c);
			if (ImGui::TreeNodeEx("choice", ImGuiTreeNodeFlags_DefaultOpen, "%s you: %s%s", match ? ">" : " ", c.text.c_str(), when.empty() ? "" : ("  [" + when + "]").c_str()))
			{
				for (const npc::Effect& e : c.effects)
					ImGui::TextColored(kMatch, "  do: %s %s %+.0f", e.kind == npc::Effect::Kind::Regard ? "friendship" : "flag", e.name.c_str(), e.amount);
				if (depth < 8) DrawSteps(c.then, context, depth + 1);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}
}

void NpcTools::DrawDialogView(const Data& data, Request& request)
{
	if (!data.dialogs) return;
	if (ImGui::SmallButton("Reload dialog and missions")) request.reload = true;
	ImGui::SameLine();
	const size_t problems = data.dialogs->Errors().size() + (data.missions ? data.missions->Errors().size() : 0);
	if (problems == 0) ImGui::TextColored(kMatch, "%zu files, no problems", data.dialogs->FileCount());
	else
	{
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%zu problem%s", problems, problems == 1 ? "" : "s");
		for (const std::string& e : data.dialogs->Errors()) Wrapped(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), e);
		if (data.missions) for (const std::string& e : data.missions->Errors()) Wrapped(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), e);
	}

	// The situation.
	bool changed = PickPerson("Talking to", mTalker);
	changed |= ImGui::Combo("Friendship", &mTier, kTiers, IM_ARRAYSIZE(kTiers));
	changed |= ImGui::Checkbox("Met", &mMet);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Rain", &mRain);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Deja vu", &mDejaVu);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Backpack has everything", &mHasEverything);
	changed |= ImGui::SliderFloat("Days away", &mAway, 0.0f, 3650.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
	changed |= ImGui::SliderFloat("Hour##talk", &mSandboxHour, 0.0f, 23.9f, "%.1f");
	changed |= ImGui::SliderFloat("Years since arrival##talk", &mSandboxYears, 0.0f, 110.0f, "%.2f");
	changed |= ImGui::InputText("Flags set", mFlags, sizeof(mFlags));
	if (changed || mSandboxLog.Events().empty()) ResetSandbox();
	const npc::DialogContext context = SandboxContext();
	ImGui::TextDisabled("%s, %s%s", Clock(context.days).c_str(), kEras[context.era], context.night ? ", night" : "");
	ImGui::Separator();

	// What they'd say.
	const npc::DialogFile* file = data.dialogs->For(mTalker);
	const npc::DialogFile* shared = data.dialogs->Shared();
	if (!file) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s has no dialog file (Content/Dialog/%s.dialog).", npc::Cast()[mTalker].name, npc::Cast()[mTalker].id);
	const std::string greeting = data.dialogs->Pick(&npc::DialogFile::greetings, context, 1u);
	ImGui::TextColored(kChosen, "Greeting now:");
	ImGui::SameLine();
	ImGui::TextWrapped("\"%s\"", greeting.empty() ? "..." : greeting.c_str());

	if (!mTalking && ImGui::Button("Try a conversation in this situation"))
	{
		ResetSandbox();
		npc::Conversation::Hooks hooks;
		const npc::MissionBook* book = data.missions;
		const npc::DialogLibrary* dialogs = data.dialogs;
		hooks.topics = [this, book, dialogs](const npc::DialogContext& c, std::deque<npc::Topic>& out)
		{
			npc::LetterTopics(*dialogs, c, out);
			if (book) book->Topics(c, out);
		};
		hooks.count = context.count;
		hooks.effect = [this, book](const npc::Effect& e)
		{
			const double days = SandboxContext().days;
			if (book && e.kind == npc::Effect::Kind::Accept) book->Accept(mSandboxLog, e.name, days);
			if (book && e.kind == npc::Effect::Kind::Complete) book->Complete(mSandboxLog, e.name, days);
		};
		mConversation.SetHooks(hooks);
		npc::Conversation::Situation situation;
		situation.days = context.days;
		situation.hour = context.hour;
		situation.rain = context.rain;
		situation.night = context.night;
		situation.era = context.era;
		mConversation.Start(*data.dialogs, mSandboxLog, mTalker, situation);
		mTalking = true;
		mChoosingGift = false;
	}
	if (mTalking) TryConversation(data);
	ImGui::Separator();

	if (file)
	{
		DrawLines("Greetings", file->greetings, context, true);
		if (shared) DrawLines("Shared greetings", shared->greetings, context, true);
		DrawLines("Barks", file->barks, context, false);
		if (shared) DrawLines("Shared barks", shared->barks, context, false);
		DrawLines("Farewells", file->farewells, context, true);
		if (ImGui::TreeNode("Gift reactions"))
		{
			for (const char* kind : { "liked", "neutral", "disliked", "again" })
			{
				npc::DialogContext g = context;
				g.gift = kind;
				const std::string reply = data.dialogs->Pick(&npc::DialogFile::gifts, g, 1u);
				ImGui::Text("%-9s \"%s\"", kind, reply.empty() ? "Thank you." : reply.c_str());
			}
			ImGui::TreePop();
		}
		if (ImGui::TreeNodeEx("Topics", ImGuiTreeNodeFlags_DefaultOpen, "Topics (%zu)", file->topics.size()))
		{
			for (const npc::Topic& t : file->topics)
			{
				const bool done = t.once && mSandboxLog.Flag(npc::DoneFlag(mTalker, t.id), context.days);
				const bool match = npc::Matches(t.when, context) && !done;
				const std::string when = npc::Describe(t.when);
				ImGui::PushStyleColor(ImGuiCol_Text, match ? kMatch : kDim);
				const bool open = ImGui::TreeNode(t.id.c_str(), "%s %s%s%s", match ? "+" : " ", t.title.c_str(), t.once ? "  (once)" : "", when.empty() ? "" : ("  [" + when + "]").c_str());
				ImGui::PopStyleColor();
				if (open)
				{
					DrawSteps(t.steps, context, 0);
					ImGui::TreePop();
				}
			}
			ImGui::TreePop();
		}
		if (!file->letters.empty() && ImGui::TreeNode("Letters"))
		{
			for (const npc::LetterDef& l : file->letters)
			{
				const std::string when = npc::Describe(l.when);
				ImGui::BulletText("on %s%s%s%s", l.on.c_str(), l.about.empty() ? "" : (" about " + l.about).c_str(),
					l.on == "away" ? (" after " + std::to_string(static_cast<int>(l.days)) + " days").c_str() : "", when.empty() ? "" : ("  [" + when + "]").c_str());
				for (const std::string& say : l.say) Wrapped(kDim, "\"" + say + "\"");
			}
			ImGui::TreePop();
		}
	}
	if (data.missions && ImGui::TreeNode("Missions on offer in this situation"))
	{
		std::deque<npc::Topic> made;
		data.missions->Topics(context, made);
		if (made.empty()) ImGui::TextDisabled("None.");
		for (const npc::Topic& t : made) ImGui::BulletText("%s", t.title.c_str());
		ImGui::TreePop();
	}
}

void NpcTools::TryConversation(const Data& data)
{
	(void)data;
	ImGui::BeginChild("tryout", ImVec2(0.0f, 170.0f), true);
	ImGui::TextColored(kChosen, "%s:", npc::Cast()[mTalker].name);
	ImGui::SameLine();
	ImGui::TextWrapped("%s", mConversation.Text().c_str());
	const npc::Standing s = mSandboxLog.StandingOf(mTalker, SandboxContext().days);
	ImGui::TextDisabled("friendship now %s (%+.0f)", npc::TierName(s.tier), s.regard);
	switch (mConversation.GetMode())
	{
	case npc::Conversation::Mode::Line:
		if (ImGui::Button("Continue")) mConversation.Continue();
		break;
	case npc::Conversation::Mode::Over:
		ImGui::TextDisabled("(the conversation is over)");
		break;
	default:
		if (mChoosingGift)
		{
			for (const cfg::BlockInfo& b : cfg::kBlocks)
			{
				if (b.basePrice <= 0 && std::string(b.name) != "Stone") continue;
				if (ImGui::SmallButton(b.name)) { mConversation.Give(b.name); mChoosingGift = false; }
				ImGui::SameLine();
			}
			if (ImGui::SmallButton("Never mind")) mChoosingGift = false;
		}
		else
		{
			const std::vector<std::string> options = mConversation.Options(true);
			for (size_t i = 0; i < options.size(); ++i)
			{
				ImGui::PushID(static_cast<int>(i));
				if (ImGui::Button(options[i].c_str()) && mConversation.Choose(static_cast<int>(i), true)) mChoosingGift = true;
				ImGui::PopID();
			}
		}
		break;
	}
	if (ImGui::SmallButton("Stop")) mTalking = false;
	ImGui::EndChild();
}

// =============================================================================
// In the editor
// =============================================================================

struct NpcToolsSystem::State
{
	Valley valley;
	ValleyProps props;
	npc::Town town;
	npc::DialogLibrary dialogs;
	npc::MissionBook missions;
	NpcTools tools;
	bool loaded = false;
	float rebuildTimer = 0.0f;
};

NpcToolsSystem::NpcToolsSystem() = default;
NpcToolsSystem::~NpcToolsSystem() = default;

void NpcToolsSystem::OnEditUpdate(Engine& engine, float deltaTime)
{
	Scene& scene = engine.GetEditScene();
	if (!scene.IsRegistered<ValleyWorld>()) return;
	bool hasValley = false;
	scene.Each<ValleyWorld>([&](Entity, ValleyWorld&) { hasValley = true; });
	if (!hasValley) return;   // other scenes (the test cave) get no people tools
	if (!mState) mState = std::make_unique<State>();
	State& s = *mState;
	if (!s.loaded)
	{
		s.dialogs.LoadFolder((engine.Project().content / "Dialog").string());
		s.missions.LoadFolder((engine.Project().content / "Missions").string());
		s.loaded = true;
	}
	// The town from the edit scene's props (rebuilt now and then: props get moved).
	s.rebuildTimer -= deltaTime;
	if (!s.town.Built() || s.rebuildTimer <= 0.0f)
	{
		s.valley.SetPads(s.props.Pads(engine, scene, s.valley));
		s.town.Build(s.valley, [&](const char* entity, XMFLOAT4X4& world)
		{
			bool found = false;
			scene.Each<EntityInfo>([&](Entity e, EntityInfo& info)
			{
				if (found || info.name != entity || !scene.Has<Transform>(e)) return;
				XMStoreFloat4x4(&world, scene.WorldMatrix(e));
				found = true;
			});
			return found;
		});
		s.rebuildTimer = 5.0f;
	}

	NpcTools::Data data;
	data.town = &s.town;
	data.dialogs = &s.dialogs;
	data.missions = &s.missions;
	NpcTools::Request request;
	// Floating at first; dock them where you like (ImGui remembers).
	ImGui::SetNextWindowSize(ImVec2(480.0f, 620.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(80.0f, 90.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("UNDERTIME: People"))
		s.tools.DrawInspector(data, &engine.GetRenderer().Debug(), request);
	ImGui::End();
	ImGui::SetNextWindowSize(ImVec2(520.0f, 680.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(580.0f, 90.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("UNDERTIME: Dialog"))
		s.tools.DrawDialogView(data, request);
	ImGui::End();
	if (request.reload) s.loaded = false;
}

void NpcToolsSystem::OnUnload(Engine&)
{
	mState.reset();
}
