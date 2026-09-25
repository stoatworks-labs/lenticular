/*
 * Stoatworks Labs - About window data for Lenticular.
 *
 * PROVISIONAL HAND COPY, written 2026-09-25 in the shape that
 * stoatworks-backend/scripts/sync-about.py generates, adapted from relay's.
 * Lenticular is not in the website's projects.json yet, so there was nothing
 * to generate it from. When it is registered, the sync overwrites this file
 * and it becomes generated like every sibling's.
 *
 * `guide` is empty on purpose: no user guide exists yet, and
 * StoatworksAboutLinks.h leaves a missing link out of the button list rather
 * than showing a button that opens a 404. `page` and `repo` point at the
 * intended homes, which do not exist yet either.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Lenticular";
    inline constexpr auto slug = "lenticular";
    inline constexpr auto hook = "A printed lenticular sheet that shows one layer or the other, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/lenticular/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/lenticular";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
