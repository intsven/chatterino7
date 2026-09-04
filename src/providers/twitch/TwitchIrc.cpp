#include "providers/twitch/TwitchIrc.hpp"

#include "Application.hpp"
#include "common/Aliases.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "util/IrcHelpers.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QEventLoop>
#include <QTimer>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

void appendTwitchEmoteOccurrences(const QString &emote,
                                  std::vector<TwitchEmoteOccurrence> &vec,
                                  const std::vector<int> &correctPositions,
                                  const QString &originalMessage,
                                  int messageOffset)
{
    auto *app = getApp();
    if (!emote.contains(':'))
    {
        return;
    }

    auto parameters = emote.split(':');

    if (parameters.length() < 2)
    {
        return;
    }

    auto id = EmoteId{parameters.at(0)};

    auto occurrences = parameters.at(1).split(',');

    for (const QString &occurrence : occurrences)
    {
        auto coords = occurrence.split('-');

        if (coords.length() < 2)
        {
            return;
        }

        auto from = coords.at(0).toUInt() - messageOffset;
        auto to = coords.at(1).toUInt() - messageOffset;
        auto maxPositions = correctPositions.size();
        if (from > to || to >= maxPositions)
        {
            // Emote coords are out of range
            qCDebug(chatterinoTwitch)
                << "Emote coords" << from << "-" << to << "are out of range ("
                << maxPositions << ")";
            return;
        }

        auto start = correctPositions[from];
        auto end = correctPositions[to];
        if (start > end || start < 0 || end > originalMessage.length())
        {
            // Emote coords are out of range from the modified character positions
            qCDebug(chatterinoTwitch) << "Emote coords" << from << "-" << to
                                      << "are out of range after offsets ("
                                      << originalMessage.length() << ")";
            return;
        }

        auto name = EmoteName{originalMessage.mid(start, end - start + 1)};
        TwitchEmoteOccurrence emoteOccurrence{
            start,
            end,
            app->getEmotes()->getTwitchEmotes()->getOrCreateEmote(id, name),
            name,
        };
        if (emoteOccurrence.ptr == nullptr)
        {
            qCDebug(chatterinoTwitch)
                << "nullptr" << emoteOccurrence.name.string;
        }
        vec.push_back(std::move(emoteOccurrence));
    }
}

}  // namespace

namespace chatterino {

std::unordered_map<QString, QString> parseBadgeInfoTag(const QVariantMap &tags)
{
    std::unordered_map<QString, QString> infoMap;

    auto infoIt = tags.constFind("badge-info");
    if (infoIt == tags.end())
    {
        return infoMap;
    }

    auto info = infoIt.value().toString().split(',', Qt::SkipEmptyParts);

    for (const QString &badge : info)
    {
        infoMap.emplace(slashKeyValue(badge));
    }

    return infoMap;
}

std::vector<Badge> parseBadgeTag(const QVariantMap &tags)
{
    std::vector<Badge> b;

    auto badgesIt = tags.constFind("badges");
    if (badgesIt == tags.end())
    {
        return b;
    }

    auto badges = badgesIt.value().toString().split(',', Qt::SkipEmptyParts);

    for (const QString &badge : badges)
    {
        if (!badge.contains('/'))
        {
            continue;
        }

        auto pair = slashKeyValue(badge);
        b.emplace_back(Badge{pair.first, pair.second});
    }

    return b;
}

std::vector<TwitchEmoteOccurrence> parseTwitchEmotes(const QVariantMap &tags,
                                                     const QString &content,
                                                     int messageOffset)
{
    // Twitch emotes
    std::vector<TwitchEmoteOccurrence> twitchEmotes;

    auto emotesTag = tags.find("emotes");

    if (emotesTag == tags.end())
    {
        return twitchEmotes;
    }

    QStringList emoteString = emotesTag.value().toString().split('/');
    std::vector<int> correctPositions;
    for (int i = 0; i < content.size(); ++i)
    {
        if (!content.at(i).isLowSurrogate())
        {
            correctPositions.push_back(i);
        }
    }
    for (const QString &emote : emoteString)
    {
        appendTwitchEmoteOccurrences(emote, twitchEmotes, correctPositions,
                                     content, messageOffset);
    }

    return twitchEmotes;
}

// GIPHY Search API key (public, read-only, from giphy.com)
static constexpr const char *GIPHY_API_KEY = "b8iXa1LndnNFn38HoeJkvp0TRprEGLI8";
static constexpr int GIPHY_SEARCH_TIMEOUT_MS = 3000;

// Synchronous GIPHY search - extracts GIF ID from search result.
// Returns empty string on failure.
QString searchGiphyForGif(const QString &query)
{
    QUrl url(u"https://api.giphy.com/v1/gifs/search"_s);
    QUrlQuery params;
    params.addQueryItem(u"api_key"_s, QString::fromUtf8(GIPHY_API_KEY));
    params.addQueryItem(u"q"_s, query);
    params.addQueryItem(u"limit"_s, u"1"_s);
    params.addQueryItem(u"rating"_s, u"pg-13"_s);
    url.setQuery(params);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);

    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(request);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(GIPHY_SEARCH_TIMEOUT_MS);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError)
    {
        qCDebug(chatterinoTwitch)
            << "GIPHY search failed:" << reply->errorString();
        reply->deleteLater();
        return {};
    }

    auto data = reply->readAll();
    reply->deleteLater();

    auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
    {
        qCDebug(chatterinoTwitch) << "GIPHY search: unexpected response";
        return {};
    }

    auto results = doc.array();
    if (results.isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "GIPHY search: no results for" << query;
        return {};
    }

    auto firstResult = results[0].toObject();
    auto id = firstResult.value("id").toString();
    qCDebug(chatterinoTwitch) << "GIPHY search result - id:" << id
                              << "for query:" << query;
    return id;
}

std::vector<TwitchGifOccurrence> parseTwitchGifs(const QVariantMap &tags,
                                                   const QString &content,
                                                   int messageOffset)
{
    std::vector<TwitchGifOccurrence> gifs;

    auto gifsTag = tags.find("gifs");
    if (gifsTag != tags.end())
    {
        auto gifsString = gifsTag.value().toString();
        qCDebug(chatterinoTwitch) << "GIFs tag found:" << gifsString;
        if (!gifsString.isEmpty())
        {
            // Format: <range>|<gifID>|<gifURL>,<range>|<gifID>|<gifURL>
            auto gifEntries = gifsString.split(',', Qt::SkipEmptyParts);
            for (const QString &entry : gifEntries)
            {
                auto parts = entry.split('|');
                qCDebug(chatterinoTwitch)
                    << "  GIF entry:" << entry << "parts:" << parts;
                if (parts.size() < 2)
                {
                    continue;
                }

                auto id = parts.at(1);
                auto url = parts.size() >= 3 ? parts.at(2) : QString();
                if (id.isEmpty())
                {
                    qCDebug(chatterinoTwitch)
                        << "  Skipping empty ID, full tag value:" << gifsString;
                    continue;
                }

                // Extract original text from range
                QString originalText;
                int startPos = -1;
                int endPos = -1;
                auto range = parts.at(0).split('-');
                if (range.size() == 2)
                {
                    auto from = range.at(0).toUInt();
                    auto to = range.at(1).toUInt();
                    startPos = static_cast<int>(from);
                    endPos = static_cast<int>(to);
                    if (to < static_cast<uint>(content.length()))
                    {
                        originalText = content.mid(
                            startPos, endPos - startPos + 1);
                    }
                }

                qCDebug(chatterinoTwitch)
                    << "  Parsed GIF - id:" << id << "url:" << url
                    << "text:" << originalText << "range:" << startPos << "-"
                    << endPos;
                gifs.push_back(TwitchGifOccurrence{
                    id, url, originalText, startPos, endPos});
            }
        }
    }
    else
    {
        // Log all available tags for debugging
        qCDebug(chatterinoTwitch)
            << "No GIFs tag. Available tags:" << tags.keys();
    }

    // Fallback: detect bracketed GIF pattern like [Title GIF by Source]
    if (gifs.empty())
    {
        static QRegularExpression gifPattern(
            u"\\[([^\\]]*?\\bGIF\\b[^\\]]*?)\\]"_s);
        auto match = gifPattern.match(content);
        if (match.hasMatch())
        {
            auto text = match.captured(1);
            auto bracketStart = static_cast<int>(match.capturedStart());
            auto bracketEnd = static_cast<int>(match.capturedEnd()) - 1;
            qCDebug(chatterinoTwitch)
                << "Fallback GIF pattern matched:" << text
                << "at range:" << bracketStart << "-" << bracketEnd;

            // Search GIPHY API using the GIF title
            // Extract title: "Shock What GIF by ZenlessZoneZero" -> "Shock What"
            QString searchQuery = text;
            static QRegularExpression titlePattern(
                QStringLiteral("^(.*?)\\s+GIF\\b"),
                QRegularExpression::CaseInsensitiveOption);
            auto titleMatch = titlePattern.match(text);
            if (titleMatch.hasMatch())
            {
                searchQuery = titleMatch.captured(1).trimmed();
            }

            qCDebug(chatterinoTwitch)
                << "Searching GIPHY for:" << searchQuery;
            auto giphyId = searchGiphyForGif(searchQuery);

            if (!giphyId.isEmpty())
            {
                qCDebug(chatterinoTwitch)
                    << "GIPHY search succeeded, ID:" << giphyId;
                gifs.push_back(TwitchGifOccurrence{
                    giphyId, {}, text, bracketStart, bracketEnd});
            }
            else
            {
                qCDebug(chatterinoTwitch)
                    << "GIPHY search failed, bracketed text stays as-is";
            }
        }
    }

    return gifs;
}

}  // namespace chatterino
