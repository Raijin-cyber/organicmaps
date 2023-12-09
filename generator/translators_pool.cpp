#include "generator/translators_pool.hpp"

#include <future>

namespace generator
{
TranslatorsPool::TranslatorsPool(std::shared_ptr<TranslatorInterface> const & original,
                                 size_t threadCount)
  : m_threadPool(threadCount)
{
  CHECK_GREATER_OR_EQUAL(threadCount, 1, ());

  m_translators.Push(original);
  for (size_t i = 1; i < threadCount; ++i)
    m_translators.Push(original->Clone());
}

void TranslatorsPool::Emit(std::vector<OsmElement> && elements)
{
  std::shared_ptr<TranslatorInterface> translator;
  m_translators.WaitAndPop(translator);
  m_threadPool.SubmitWork([&, translator, elements = std::move(elements)]() mutable
  {
    for (auto const & element : elements)
      translator->Emit(element);

    m_translators.Push(translator);
  });
}

bool TranslatorsPool::Finish()
{
  m_threadPool.WaitingStop();
  using TranslatorPtr = std::shared_ptr<TranslatorInterface>;
  threads::ThreadSafeQueue<std::future<TranslatorPtr>> queue;
  while (!m_translators.Empty())
  {
    std::promise<TranslatorPtr> p;
    std::shared_ptr<TranslatorInterface> translator;
    m_translators.TryPop(translator);
    p.set_value(translator);
    queue.Push(p.get_future());
  }

  base::thread_pool::computational::ThreadPool pool(queue.Size() / 2 + 1);
  CHECK_GREATER_OR_EQUAL(queue.Size(), 1, ());
  while (queue.Size() != 1)
  {
    std::future<TranslatorPtr> left;
    std::future<TranslatorPtr> right;
    queue.WaitAndPop(left);
    queue.WaitAndPop(right);
    // ThreadPool uses packaged_task<> to wrap lambda. MSVC compiler requires lambda to be copyable.
    // But std::future is not copyable. Need to wrap 'left' and 'right' futures into shared_ptr
    // (It's known behaviour of MSVC: https://stackoverflow.com/a/48999075)
    auto leftPtr = std::make_shared<std::future<TranslatorPtr>>(std::move(left));
    auto rightPtr = std::make_shared<std::future<TranslatorPtr>>(std::move(right));
    queue.Push(pool.Submit([left = leftPtr, right = rightPtr]() mutable {
      auto leftTranslator = left->get();
      auto rigthTranslator = right->get();
      rigthTranslator->Finish();
      leftTranslator->Finish();
      leftTranslator->Merge(*rigthTranslator);
      return leftTranslator;
    }));
  }

  std::future<TranslatorPtr> translatorFuture;
  queue.WaitAndPop(translatorFuture);
  auto translator = translatorFuture.get();
  translator->Finish();
  return translator->Save();
}
}  // namespace generator
