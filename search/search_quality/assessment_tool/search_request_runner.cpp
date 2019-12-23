#include "search/search_quality/assessment_tool/search_request_runner.hpp"

#include "search/feature_loader.hpp"

#include "base/assert.hpp"
#include "base/logging.hpp"

#include <chrono>
#include <utility>

using namespace std;

SearchRequestRunner::SearchRequestRunner(Framework & framework, DataSource const & dataSource,
                                         ContextList & contexts,
                                         UpdateViewOnResults && updateViewOnResults,
                                         UpdateSampleSearchState && updateSampleSearchState)
  : m_framework(framework)
  , m_dataSource(dataSource)
  , m_contexts(contexts)
  , m_updateViewOnResults(move(updateViewOnResults))
  , m_updateSampleSearchState(move(updateSampleSearchState))
{
}

void SearchRequestRunner::InitiateForegroundSearch(size_t index)
{
  RunRequest(index, false /* background */, m_foregroundTimestamp);
}

void SearchRequestRunner::InitiateBackgroundSearch(size_t from, size_t to)
{
  // 1 <= from <= to <= m_contexts.Size().
  if (from < 1 || from > to || to > m_contexts.Size())
  {
    LOG(LINFO,
        ("Could not initiate search in the range", from, to, "Total samples:", m_contexts.Size()));
    return;
  }

  ResetBackgroundSearch();

  // Convert to 0-based.
  --from;
  --to;
  m_backgroundFirstIndex = from;
  m_backgroundLastIndex = to;
  m_numProcessedRequests = 0;

  for (size_t index = from; index <= to; ++index)
  {
    if (m_contexts[index].m_searchState == Context::SearchState::Untouched)
    {
      m_contexts[index].m_searchState = Context::SearchState::InQueue;
      m_backgroundQueue.push(index);
      m_updateSampleSearchState(index);
    }
    else
    {
      CHECK(m_contexts[index].m_searchState == Context::SearchState::Completed, ());
    }
  }

  m_vitalsInLastBackgroundSearch.clear();
  RunNextBackgroundRequest(m_backgroundTimestamp);
}

void SearchRequestRunner::RunNextBackgroundRequest(size_t timestamp)
{
  // todo(@m) Process in batches instead?
  if (m_backgroundQueue.empty())
  {
    LOG(LINFO, ("All requests from", m_backgroundFirstIndex + 1, "to", m_backgroundLastIndex + 1,
                "have been processed"));
    LOG(LINFO, ("Vital results found:", m_vitalsInLastBackgroundSearch.size(), "out of",
                m_backgroundLastIndex - m_backgroundFirstIndex + 1));
    LOG(LINFO,
        ("Vital results found for these queries (1-based):", m_vitalsInLastBackgroundSearch));
    return;
  }
  size_t index = m_backgroundQueue.front();
  m_backgroundQueue.pop();

  RunRequest(index, true /* background */, timestamp);
}

void SearchRequestRunner::RunRequest(size_t index, bool background, size_t timestamp)
{
  CHECK_THREAD_CHECKER(m_threadChecker, ());

  auto const & context = m_contexts[index];
  auto const & sample = context.m_sample;

  // todo(@m) What if we want multiple threads in engine?
  auto & engine = m_framework.GetSearchAPI().GetEngine();

  search::SearchParams params;
  sample.FillSearchParams(params);
  params.m_timeout = search::SearchParams::kDefaultDesktopTimeout;
  params.m_onResults = [=](search::Results const & results) {
    vector<optional<ResultsEdits::Relevance>> relevances;
    vector<size_t> goldenMatching;
    vector<size_t> actualMatching;

    if (results.IsEndedNormal())
    {
      // Can't use MainModel's m_loader here due to thread-safety issues.
      search::FeatureLoader loader(m_dataSource);
      search::Matcher matcher(loader);

      vector<search::Result> const actual(results.begin(), results.end());
      matcher.Match(sample, actual, goldenMatching, actualMatching);
      relevances.resize(actual.size());
      bool foundVital = false;
      for (size_t i = 0; i < goldenMatching.size(); ++i)
      {
        auto const j = goldenMatching[i];
        if (j != search::Matcher::kInvalidId)
        {
          CHECK_LESS(j, relevances.size(), ());
          relevances[j] = sample.m_results[i].m_relevance;

          if (relevances[j] == search::Sample::Result::Relevance::Vital)
          {
            foundVital = true;
          }
        }
      }

      if (foundVital)
        m_vitalsInLastBackgroundSearch.emplace_back(index + 1);

      LOG(LINFO, ("Request number", index + 1, "has been processed in the",
                  background ? "background" : "foreground"));
    }

    GetPlatform().RunTask(Platform::Thread::Gui, [this, background, timestamp, index, results,
                                                  relevances, goldenMatching, actualMatching] {
      size_t const latestTimestamp = background ? m_backgroundTimestamp : m_foregroundTimestamp;
      if (timestamp != latestTimestamp)
        return;

      auto & context = m_contexts[index];

      context.m_foundResults = results;

      if (results.IsEndMarker())
      {
        if (results.IsEndedNormal())
          context.m_searchState = Context::SearchState::Completed;
        else
          context.m_searchState = Context::SearchState::Untouched;
        m_updateSampleSearchState(index);
      }

      if (results.IsEndedNormal())
      {
        if (!context.m_initialized)
        {
          context.m_foundResultsEdits.Reset(relevances);
          context.m_goldenMatching = goldenMatching;
          context.m_actualMatching = actualMatching;

          {
            vector<optional<ResultsEdits::Relevance>> relevances;

            auto & nonFound = context.m_nonFoundResults;
            CHECK(nonFound.empty(), ());
            for (size_t i = 0; i < context.m_goldenMatching.size(); ++i)
            {
              auto const j = context.m_goldenMatching[i];
              if (j != search::Matcher::kInvalidId)
                continue;
              nonFound.push_back(context.m_sample.m_results[i]);
              relevances.emplace_back(nonFound.back().m_relevance);
            }
            context.m_nonFoundResultsEdits.Reset(relevances);
          }

          context.m_initialized = true;
        }

        if (background)
          RunNextBackgroundRequest(timestamp);
      }

      if (!background)
        m_updateViewOnResults(results);
    });
  };

  if (background)
    m_backgroundQueryHandle = engine.Search(params);
  else
    m_foregroundQueryHandle = engine.Search(params);
}

void SearchRequestRunner::ResetForegroundSearch()
{
  CHECK_THREAD_CHECKER(m_threadChecker, ());

  ++m_foregroundTimestamp;
  if (auto handle = m_foregroundQueryHandle.lock())
    handle->Cancel();
}

void SearchRequestRunner::ResetBackgroundSearch()
{
  CHECK_THREAD_CHECKER(m_threadChecker, ());

  ++m_backgroundTimestamp;
  auto handle = m_backgroundQueryHandle.lock();
  if (!handle)
    return;

  handle->Cancel();

  for (size_t index = m_backgroundFirstIndex; index <= m_backgroundLastIndex; ++index)
  {
    if (m_contexts[index].m_searchState == Context::SearchState::InQueue)
    {
      m_contexts[index].m_searchState = Context::SearchState::Untouched;
      m_updateSampleSearchState(index);
    }
  }

  queue<size_t>().swap(m_backgroundQueue);
}
