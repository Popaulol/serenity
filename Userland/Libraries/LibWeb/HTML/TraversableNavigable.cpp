/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(TraversableNavigable);

TraversableNavigable::TraversableNavigable(JS::NonnullGCPtr<Page> page)
    : m_page(page)
{
}

TraversableNavigable::~TraversableNavigable() = default;

void TraversableNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    for (auto& entry : m_session_history_entries)
        visitor.visit(entry);
}

static OrderedHashTable<TraversableNavigable*>& user_agent_top_level_traversable_set()
{
    static OrderedHashTable<TraversableNavigable*> set;
    return set;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_top_level_browsing_context_and_document(JS::NonnullGCPtr<Page> page)
{
    // 1. Let group and document be the result of creating a new browsing context group and document.
    auto [group, document] = TRY(BrowsingContextGroup::create_a_new_browsing_context_group_and_document(page));

    // 2. Return group's browsing context set[0] and document.
    return BrowsingContextAndDocument { **group->browsing_context_set().begin(), document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
WebIDL::ExceptionOr<JS::NonnullGCPtr<TraversableNavigable>> TraversableNavigable::create_a_new_top_level_traversable(JS::NonnullGCPtr<Page> page, JS::GCPtr<HTML::BrowsingContext> opener, String target_name)
{
    auto& vm = Bindings::main_thread_vm();

    // 1. Let document be null.
    JS::GCPtr<DOM::Document> document = nullptr;

    // 2. If opener is null, then set document to the second return value of creating a new top-level browsing context and document.
    if (!opener) {
        document = TRY(create_a_new_top_level_browsing_context_and_document(page)).document;
    }

    // 3. Otherwise, set document to the second return value of creating a new auxiliary browsing context and document given opener.
    else {
        document = TRY(BrowsingContext::create_a_new_auxiliary_browsing_context_and_document(page, *opener)).document;
    }

    // 4. Let documentState be a new document state, with
    auto document_state = vm.heap().allocate_without_realm<DocumentState>();

    // document: document
    document_state->set_document(document);

    // initiator origin: null if opener is null; otherwise, document's origin
    document_state->set_initiator_origin(opener ? Optional<Origin> {} : document->origin());

    // origin: document's origin
    document_state->set_origin(document->origin());

    // navigable target name: targetName
    document_state->set_navigable_target_name(target_name);

    // about base URL: document's about base URL
    document_state->set_about_base_url(document->about_base_url());

    // 5. Let traversable be a new traversable navigable.
    auto traversable = vm.heap().allocate_without_realm<TraversableNavigable>(page);

    // 6. Initialize the navigable traversable given documentState.
    TRY_OR_THROW_OOM(vm, traversable->initialize_navigable(document_state, nullptr));

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    auto initial_history_entry = traversable->active_session_history_entry();
    VERIFY(initial_history_entry);

    // 8. Set initialHistoryEntry's step to 0.
    initial_history_entry->step = 0;

    // 9. Append initialHistoryEntry to traversable's session history entries.
    traversable->m_session_history_entries.append(*initial_history_entry);

    // FIXME: 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]

    // 11. Append traversable to the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().set(traversable);

    // 12. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-fresh-top-level-traversable
WebIDL::ExceptionOr<JS::NonnullGCPtr<TraversableNavigable>> TraversableNavigable::create_a_fresh_top_level_traversable(JS::NonnullGCPtr<Page> page, AK::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> initial_navigation_post_resource)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = TRY(create_a_new_top_level_traversable(page, nullptr, {}));
    page->set_top_level_traversable(traversable);

    // AD-HOC: Mark the about:blank document as finished parsing if we're only going to about:blank
    //         Skip the initial navigation as well. This matches the behavior of the window open steps.

    if (url_matches_about_blank(initial_navigation_url)) {
        Platform::EventLoopPlugin::the().deferred_invoke([traversable, initial_navigation_url] {
            // FIXME: We do this other places too when creating a new about:blank document. Perhaps it's worth a spec issue?
            HTML::HTMLParser::the_end(*traversable->active_document());

            // FIXME: If we perform the URL and history update steps here, we start hanging tests and the UI process will
            //        try to load() the initial URLs passed on the command line before we finish processing the events here.
            //        However, because we call this before the PageClient is fully initialized... that gets awkward.
        });
    }

    else {
        // 2. Navigate traversable to initialNavigationURL using traversable's active document, with documentResource set to initialNavigationPostResource.
        TRY(traversable->navigate({ .url = initial_navigation_url,
            .source_document = *traversable->active_document(),
            .document_resource = initial_navigation_post_resource }));
    }

    // 3. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#top-level-traversable
bool TraversableNavigable::is_top_level_traversable() const
{
    // A top-level traversable is a traversable navigable with a null parent.
    return parent() == nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
Vector<int> TraversableNavigable::get_all_used_history_steps() const
{
    // FIXME: 1. Assert: this is running within traversable's session history traversal queue.

    // 2. Let steps be an empty ordered set of non-negative integers.
    OrderedHashTable<int> steps;

    // 3. Let entryLists be the ordered set « traversable's session history entries ».
    Vector<Vector<JS::NonnullGCPtr<SessionHistoryEntry>>> entry_lists { session_history_entries() };

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto entry_list = entry_lists.take_first();

        // 1. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. Append entry's step to steps.
            steps.set(entry->step.get<int>());

            // 2. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state->nested_histories())
                entry_lists.append(nested_history.entries);
        }
    }

    // 5. Return steps, sorted.
    auto sorted_steps = steps.values();
    quick_sort(sorted_steps);
    return sorted_steps;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-history-object-length-and-index
TraversableNavigable::HistoryObjectLengthAndIndex TraversableNavigable::get_the_history_object_length_and_index(int step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps();

    // 2. Let scriptHistoryLength be the size of steps.
    auto script_history_length = steps.size();

    // 3. Assert: steps contains step.
    VERIFY(steps.contains_slow(step));

    // 4. Let scriptHistoryIndex be the index of step in steps.
    auto script_history_index = *steps.find_first_index(step);

    // 5. Return (scriptHistoryLength, scriptHistoryIndex).
    return HistoryObjectLengthAndIndex {
        .script_history_length = script_history_length,
        .script_history_index = script_history_index
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
int TraversableNavigable::get_the_used_step(int step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps();

    // 2. Return the greatest item in steps that is less than or equal to step.
    VERIFY(!steps.is_empty());
    Optional<int> result;
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i] <= step) {
            if (!result.has_value() || (result.value() < steps[i])) {
                result = steps[i];
            }
        }
    }
    return result.value();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#get-all-navigables-whose-current-session-history-entry-will-change-or-reload
Vector<JS::Handle<Navigable>> TraversableNavigable::get_all_navigables_whose_current_session_history_entry_will_change_or_reload(int target_step) const
{
    // 1. Let results be an empty list.
    Vector<JS::Handle<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<JS::Handle<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry is not navigable's current session history entry or targetEntry's document state's reload pending is true, then append navigable to results.
        if (target_entry != navigable->current_session_history_entry() || target_entry->document_state->reload_pending()) {
            results.append(*navigable);
        }

        // 3. If targetEntry's document is navigable's document, and targetEntry's document state's reload pending is false, then extend navigablesToCheck with the child navigables of navigable.
        if (target_entry->document_state->document() == navigable->active_document() && !target_entry->document_state->reload_pending()) {
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-only-need-history-object-length/index-update
Vector<JS::Handle<Navigable>> TraversableNavigable::get_all_navigables_that_only_need_history_object_length_index_update(int target_step) const
{
    // NOTE: Other navigables might not be impacted by the traversal. For example, if the response is a 204, the currently active document will remain.
    //       Additionally, going 'back' after a 204 will change the current session history entry, but the active session history entry will already be correct.

    // 1. Let results be an empty list.
    Vector<JS::Handle<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<JS::Handle<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry is navigable's current session history entry and targetEntry's document state's reload pending is false, then:
        if (target_entry == navigable->current_session_history_entry() && !target_entry->document_state->reload_pending()) {
            // 1.  Append navigable to results.
            results.append(navigable);

            // 2. Extend navigablesToCheck with navigable's child navigables.
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-might-experience-a-cross-document-traversal
Vector<JS::Handle<Navigable>> TraversableNavigable::get_all_navigables_that_might_experience_a_cross_document_traversal(int target_step) const
{
    // NOTE: From traversable's session history traversal queue's perspective, these documents are candidates for going cross-document during the
    //       traversal described by targetStep. They will not experience a cross-document traversal if the status code for their target document is
    //       HTTP 204 No Content.
    //       Note that if a given navigable might experience a cross-document traversal, this algorithm will return navigable but not its child navigables.
    //       Those would end up unloaded, not traversed.

    // 1. Let results be an empty list.
    Vector<JS::Handle<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<JS::Handle<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry's document is not navigable's document or targetEntry's document state's reload pending is true, then append navigable to results.
        // NOTE: Although navigable's active history entry can change synchronously, the new entry will always have the same Document,
        //       so accessing navigable's document is reliable.
        if (target_entry->document_state->document() != navigable->active_document() || target_entry->document_state->reload_pending()) {
            results.append(navigable);
        }

        // 3. Otherwise, extend navigablesToCheck with navigable's child navigables.
        //    Adding child navigables to navigablesToCheck means those navigables will also be checked by this loop.
        //    Child navigables are only checked if the navigable's active document will not change as part of this traversal.
        else {
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_history_step(
    int step,
    bool check_for_cancelation,
    Optional<SourceSnapshotParams> source_snapshot_params,
    JS::GCPtr<Navigable> initiator_to_check,
    Optional<UserNavigationInvolvement> user_involvement_for_navigate_events)
{
    auto& vm = this->vm();
    // FIXME: 1. Assert: This is running within traversable's session history traversal queue.

    // 2. Let targetStep be the result of getting the used step given traversable and step.
    auto target_step = get_the_used_step(step);

    // Note: Calling this early so we can re-use the same list in 3.2 and 6.
    auto change_or_reload_navigables = get_all_navigables_whose_current_session_history_entry_will_change_or_reload(target_step);

    // 3. If initiatorToCheck is not null, then:
    if (initiator_to_check != nullptr) {
        // 1. Assert: sourceSnapshotParams is not null.
        VERIFY(source_snapshot_params.has_value());

        // 2. For each navigable of get all navigables whose current session history entry will change or reload:
        //    if initiatorToCheck is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then return "initiator-disallowed".
        for (auto const& navigable : change_or_reload_navigables) {
            if (!initiator_to_check->allowed_by_sandboxing_to_navigate(*navigable, *source_snapshot_params))
                return HistoryStepResult::InitiatorDisallowed;
        }
    }

    // 4. Let navigablesCrossingDocuments be the result of getting all navigables that might experience a cross-document traversal given traversable and targetStep.
    [[maybe_unused]] auto navigables_crossing_documents = get_all_navigables_that_might_experience_a_cross_document_traversal(target_step);

    // 5. FIXME: If checkForCancelation is true, and the result of checking if unloading is canceled given navigablesCrossingDocuments, traversable, targetStep,
    //           and userInvolvementForNavigateEvents is not "continue", then return that result.
    (void)check_for_cancelation;

    // 6. Let changingNavigables be the result of get all navigables whose current session history entry will change or reload given traversable and targetStep.
    auto changing_navigables = move(change_or_reload_navigables);

    // 7. Let nonchangingNavigablesThatStillNeedUpdates be the result of getting all navigables that only need history object length/index update given traversable and targetStep.
    auto non_changing_navigables_that_still_need_updates = get_all_navigables_that_only_need_history_object_length_index_update(target_step);

    // 8. For each navigable of changingNavigables:
    for (auto& navigable : changing_navigables) {
        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. Set navigable's current session history entry to targetEntry.
        navigable->set_current_session_history_entry(target_entry);

        // 3. Set navigable's ongoing navigation to "traversal".
        navigable->set_ongoing_navigation(Traversal::Tag);
    }

    // 9. Let totalChangeJobs be the size of changingNavigables.
    auto total_change_jobs = changing_navigables.size();

    // 10. Let completedChangeJobs be 0.
    size_t completed_change_jobs = 0;

    struct ChangingNavigableContinuationState {
        JS::Handle<DOM::Document> displayed_document;
        JS::Handle<SessionHistoryEntry> target_entry;
        JS::Handle<Navigable> navigable;
        bool update_only = false;
    };

    // 11. Let changingNavigableContinuations be an empty queue of changing navigable continuation states.
    // NOTE: This queue is used to split the operations on changingNavigables into two parts. Specifically, changingNavigableContinuations holds data for the second part.
    Queue<ChangingNavigableContinuationState> changing_navigable_continuations;

    // 12. For each navigable of changingNavigables, queue a global task on the navigation and traversal task source of navigable's active window to run the steps:
    for (auto& navigable : changing_navigables) {
        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), [&] {
            // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
            if (navigable->has_been_destroyed())
                return;

            // 1. Let displayedEntry be navigable's active session history entry.
            auto displayed_entry = navigable->active_session_history_entry();

            // 2. Let targetEntry be navigable's current session history entry.
            auto target_entry = navigable->current_session_history_entry();

            // 3. Let changingNavigableContinuation be a changing navigable continuation state with:
            auto changing_navigable_continuation = ChangingNavigableContinuationState {
                .displayed_document = displayed_entry->document_state->document(),
                .target_entry = target_entry,
                .navigable = navigable,
                .update_only = false
            };

            // 4. If displayedEntry is targetEntry and targetEntry's document state's reload pending is false, then:
            if (displayed_entry == target_entry && !target_entry->document_state->reload_pending()) {
                // 1. Set changingNavigableContinuation's update-only to true.
                changing_navigable_continuation.update_only = true;

                // 2. Enqueue changingNavigableContinuation on changingNavigableContinuations.
                changing_navigable_continuations.enqueue(move(changing_navigable_continuation));

                // 3. Abort these steps.
                return;
            }

            // 5. Let oldOrigin be targetEntry's document state's origin.
            auto old_origin = target_entry->document_state->origin();

            auto after_document_populated = [old_origin, target_entry, changing_navigable_continuation, &changing_navigable_continuations, &vm, &navigable]() mutable {
                // 1. If targetEntry's document is null, then set changingNavigableContinuation's update-only to true.
                if (!target_entry->document_state->document()) {
                    changing_navigable_continuation.update_only = true;
                }

                else {
                    // 2. If targetEntry's document's origin is not oldOrigin, then set targetEntry's classic history API state to StructuredSerializeForStorage(null).
                    if (target_entry->document_state->document()->origin() != old_origin) {
                        target_entry->classic_history_api_state = MUST(structured_serialize_for_storage(vm, JS::js_null()));
                    }

                    // 3. If all of the following are true:
                    //     - navigable's parent is null;
                    //     - targetEntry's document's browsing context is not an auxiliary browsing context whose opener browsing context is non-null; and
                    //     - targetEntry's document's origin is not oldOrigin,
                    //    then set targetEntry's document state's navigable target name to the empty string.
                    if (navigable->parent() != nullptr
                        && target_entry->document_state->document()->browsing_context()->opener_browsing_context() == nullptr
                        && target_entry->document_state->origin() != old_origin) {
                        target_entry->document_state->set_navigable_target_name(String {});
                    }
                }

                // 4. Enqueue changingNavigableContinuation on changingNavigableContinuations.
                changing_navigable_continuations.enqueue(move(changing_navigable_continuation));
            };

            // 6. If targetEntry's document is null, or targetEntry's document state's reload pending is true, then:
            if (!target_entry->document_state->document() || target_entry->document_state->reload_pending()) {
                // FIXME: 1. Let navTimingType be "back_forward" if targetEntry's document is null; otherwise "reload".

                // 2. Let targetSnapshotParams be the result of snapshotting target snapshot params given navigable.
                auto target_snapshot_params = navigable->snapshot_target_snapshot_params();

                // 3. Let potentiallyTargetSpecificSourceSnapshotParams be sourceSnapshotParams.
                Optional<SourceSnapshotParams> potentially_target_specific_source_snapshot_params = source_snapshot_params;

                // 4. If potentiallyTargetSpecificSourceSnapshotParams is null, then set it to the result of snapshotting source snapshot params given navigable's active document.
                if (!potentially_target_specific_source_snapshot_params.has_value()) {
                    potentially_target_specific_source_snapshot_params = navigable->active_document()->snapshot_source_snapshot_params();
                }

                // 5. Set targetEntry's document state's reload pending to false.
                target_entry->document_state->set_reload_pending(false);

                // 6. Let allowPOST be targetEntry's document state's reload pending.
                auto allow_POST = target_entry->document_state->reload_pending();

                // 7. In parallel, attempt to populate the history entry's document for targetEntry, given navigable, potentiallyTargetSpecificSourceSnapshotParams,
                //    targetSnapshotParams, with allowPOST set to allowPOST and completionSteps set to queue a global task on the navigation and traversal task source given
                //    navigable's active window to run afterDocumentPopulated.
                Platform::EventLoopPlugin::the().deferred_invoke([target_entry, potentially_target_specific_source_snapshot_params, target_snapshot_params, this, allow_POST, navigable, after_document_populated] {
                    navigable->populate_session_history_entry_document(target_entry, *potentially_target_specific_source_snapshot_params, target_snapshot_params, {}, Empty {}, CSPNavigationType::Other, allow_POST, [this, after_document_populated]() mutable {
                                 queue_global_task(Task::Source::NavigationAndTraversal, *active_window(), [after_document_populated]() mutable {
                                     after_document_populated();
                                 });
                             })
                        .release_value_but_fixme_should_propagate_errors();
                });
            }
            // Otherwise, run afterDocumentPopulated immediately.
            else {
                after_document_populated();
            }
        });
    }

    // 13. Let navigablesThatMustWaitBeforeHandlingSyncNavigation be an empty set.
    Vector<JS::GCPtr<Navigable>> navigables_that_must_wait_before_handling_sync_navigation;

    // 14. While completedChangeJobs does not equal totalChangeJobs:
    while (completed_change_jobs != total_change_jobs) {
        // NOTE: Synchronous navigations that are intended to take place before this traversal jump the queue at this point,
        //       so they can be added to the correct place in traversable's session history entries before this traversal
        //       potentially unloads their document. More details can be found here (https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigation-steps-queue-jumping-examples)
        // 1. If traversable's running nested apply history step is false, then:
        if (!m_running_nested_apply_history_step) {
            // 1. While traversable's session history traversal queue's algorithm set contains one or more synchronous
            //    navigation steps with a target navigable not contained in navigablesThatMustWaitBeforeHandlingSyncNavigation:
            //   1. Let steps be the first item in traversable's session history traversal queue's algorithm set
            //    that is synchronous navigation steps with a target navigable not contained in navigablesThatMustWaitBeforeHandlingSyncNavigation.
            //   2. Remove steps from traversable's session history traversal queue's algorithm set.
            for (auto steps = m_session_history_traversal_queue.first_synchronous_navigation_steps_with_target_navigable_not_contained_in(navigables_that_must_wait_before_handling_sync_navigation);
                 steps.target_navigable != nullptr;
                 steps = m_session_history_traversal_queue.first_synchronous_navigation_steps_with_target_navigable_not_contained_in(navigables_that_must_wait_before_handling_sync_navigation)) {

                // 3. Set traversable's running nested apply history step to true.
                m_running_nested_apply_history_step = true;

                // 4. Run steps.
                steps.steps();

                // 5. Set traversable's running nested apply history step to false.
                m_running_nested_apply_history_step = false;
            }
        }

        // AD-HOC: Since currently populate_session_history_entry_document does not run in parallel
        //         we call spin_until to interrupt execution of this function and let document population
        //         to complete.
        Platform::EventLoopPlugin::the().spin_until([&] {
            return !changing_navigable_continuations.is_empty() || completed_change_jobs == total_change_jobs;
        });

        if (changing_navigable_continuations.is_empty()) {
            continue;
        }

        // 2. Let changingNavigableContinuation be the result of dequeuing from changingNavigableContinuations.
        auto changing_navigable_continuation = changing_navigable_continuations.dequeue();

        // 3. If changingNavigableContinuation is nothing, then continue.

        // 4. Let displayedDocument be changingNavigableContinuation's displayed document.
        auto displayed_document = changing_navigable_continuation.displayed_document;

        // 5. Let targetEntry be changingNavigableContinuation's target entry.
        auto target_entry = changing_navigable_continuation.target_entry;

        // 6. Let navigable be changingNavigableContinuation's navigable.
        auto navigable = changing_navigable_continuation.navigable;

        // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
        if (navigable->has_been_destroyed())
            continue;

        // 7. Set navigable's ongoing navigation to null.
        navigable->set_ongoing_navigation({});

        // 8. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
        auto history_object_length_and_index = get_the_history_object_length_and_index(target_step);
        auto script_history_length = history_object_length_and_index.script_history_length;
        auto script_history_index = history_object_length_and_index.script_history_index;

        // 9. Append navigable to navigablesThatMustWaitBeforeHandlingSyncNavigation.
        navigables_that_must_wait_before_handling_sync_navigation.append(*navigable);

        // 10. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API given navigable and targetStep.
        auto entries_for_navigation_api = get_session_history_entries_for_the_navigation_api(*navigable, target_step);

        // 11. Queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), [&completed_change_jobs, target_entry, navigable, displayed_document, update_only = changing_navigable_continuation.update_only, script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), user_involvement_for_navigate_events]() mutable {
            // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
            if (navigable->has_been_destroyed()) {
                return;
            }

            // 1. If changingNavigableContinuation's update-only is false, then:
            if (!update_only) {
                // 1. If targetEntry's document does not equal displayedDocument, then:
                if (target_entry->document_state->document().ptr() != displayed_document.ptr()) {
                    // 1. Unload displayedDocument given targetEntry's document.
                    displayed_document->unload(target_entry->document_state->document());

                    // 2. For each childNavigable of displayedDocument's descendant navigables, queue a global task on the navigation and traversal task source given
                    //    childNavigable's active window to unload childNavigable's active document.
                    for (auto child_navigable : displayed_document->descendant_navigables()) {
                        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), [child_navigable] {
                            child_navigable->active_document()->unload();
                        });
                    }
                }

                // 3. Activate history entry targetEntry for navigable.
                navigable->activate_history_entry(*target_entry);
            }

            // 2. If navigable is not traversable, and targetEntry is not navigable's current session history entry, and targetEntry's document state's origin is the same as
            //    navigable's current session history entry's document state's origin, then fire a traverse navigate event given targetEntry and userInvolvementForNavigateEvents.
            auto target_origin = target_entry->document_state->origin();
            auto current_origin = navigable->current_session_history_entry()->document_state->origin();
            bool const is_same_origin = target_origin.has_value() && current_origin.has_value() && target_origin->is_same_origin(*current_origin);
            if (!navigable->is_traversable()
                && target_entry.ptr() != navigable->current_session_history_entry()
                && is_same_origin) {
                navigable->active_window()->navigation()->fire_a_traverse_navigate_event(*target_entry, user_involvement_for_navigate_events.value_or(UserNavigationInvolvement::None));
            }

            // 3. Let updateDocument be an algorithm step which performs update document for history step application given targetEntry's document,
            //    targetEntry, changingNavigableContinuation's update-only, scriptHistoryLength, scriptHistoryIndex, and entriesForNavigationAPI.
            auto update_document = JS::SafeFunction<void()>([target_entry, update_only, script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api)] {
                target_entry->document_state->document()->update_for_history_step_application(*target_entry, update_only, script_history_length, script_history_index, entries_for_navigation_api);
            });

            // 4. If targetEntry's document is equal to displayedDocument, then perform updateDocument.
            if (target_entry->document_state->document() == displayed_document.ptr()) {
                update_document();
            }
            // 5. Otherwise, queue a global task on the navigation and traversal task source given targetEntry's document's relevant global object to perform updateDocument
            else {
                queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*target_entry->document_state->document()), move(update_document));
            }

            // 6. Increment completedChangeJobs.
            completed_change_jobs++;
        });
    }

    // 15. Let totalNonchangingJobs be the size of nonchangingNavigablesThatStillNeedUpdates.
    auto total_non_changing_jobs = non_changing_navigables_that_still_need_updates.size();

    // 16. Let completedNonchangingJobs be 0.
    auto completed_non_changing_jobs = 0u;

    // 17. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
    auto length_and_index = get_the_history_object_length_and_index(target_step);
    auto script_history_length = length_and_index.script_history_length;
    auto script_history_index = length_and_index.script_history_index;

    // 18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
    for (auto& navigable : non_changing_navigables_that_still_need_updates) {
        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), [&] {
            // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
            if (navigable->has_been_destroyed()) {
                ++completed_non_changing_jobs;
                return;
            }

            // 1. Let document be navigable's active document.
            auto document = navigable->active_document();

            // 2. Set document's history object's index to scriptHistoryIndex.
            document->history()->m_index = script_history_index;

            // 3. Set document's history object's length to scriptHistoryLength.
            document->history()->m_length = script_history_length;

            // 4. Increment completedNonchangingJobs.
            ++completed_non_changing_jobs;
        });
    }

    // 19. Wait for completedNonchangingJobs to equal totalNonchangingJobs.
    // AD-HOC: Since currently populate_session_history_entry_document does not run in parallel
    //         we call spin_until to interrupt execution of this function and let document population
    //         to complete.
    Platform::EventLoopPlugin::the().spin_until([&] {
        return completed_non_changing_jobs == total_non_changing_jobs;
    });

    // 20. Set traversable's current session history step to targetStep.
    m_current_session_history_step = target_step;

    // 21. Return "applied".
    return HistoryStepResult::Applied;
}

Vector<JS::NonnullGCPtr<SessionHistoryEntry>> TraversableNavigable::get_session_history_entries_for_the_navigation_api(JS::NonnullGCPtr<Navigable> navigable, int target_step)
{
    // 1. Let rawEntries be the result of getting session history entries for navigable.
    auto raw_entries = navigable->get_session_history_entries();

    if (raw_entries.is_empty())
        return {};

    // 2. Let entriesForNavigationAPI be a new empty list.
    Vector<JS::NonnullGCPtr<SessionHistoryEntry>> entries_for_navigation_api;

    // 3. Let startingIndex be the index of the session history entry in rawEntries who has the greatest step less than or equal to targetStep.
    // FIXME: Use min/max_element algorithm or some such here
    int starting_index = 0;
    auto max_step = 0;
    for (auto i = 0u; i < raw_entries.size(); ++i) {
        auto const& entry = raw_entries[i];
        if (entry->step.has<int>()) {
            auto step = entry->step.get<int>();
            if (step <= target_step && step > max_step) {
                starting_index = static_cast<int>(i);
            }
        }
    }

    // 4. Append rawEntries[startingIndex] to entriesForNavigationAPI.
    entries_for_navigation_api.append(raw_entries[starting_index]);

    // 5. Let startingOrigin be rawEntries[startingIndex]'s document state's origin.
    auto starting_origin = raw_entries[starting_index]->document_state->origin();

    // 6. Let i be startingIndex − 1.
    auto i = starting_index - 1;

    // 7. While i > 0:
    while (i > 0) {
        auto& entry = raw_entries[static_cast<unsigned>(i)];
        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto entry_origin = entry->document_state->origin();
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;

        // 2. Prepend rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.prepend(entry);

        // 3. Set i to i − 1.
        --i;
    }

    // 8. Set i to startingIndex + 1.
    i = starting_index + 1;

    // 9. While i < rawEntries's size:
    while (i < static_cast<int>(raw_entries.size())) {
        auto& entry = raw_entries[static_cast<unsigned>(i)];
        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto entry_origin = entry->document_state->origin();
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;

        // 2. Append rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.append(entry);

        // 3. Set i to i + 1.
        ++i;
    }

    // 10. Return entriesForNavigationAPI.
    return entries_for_navigation_api;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#clear-the-forward-session-history
void TraversableNavigable::clear_the_forward_session_history()
{
    // FIXME: 1. Assert: this is running within navigable's session history traversal queue.

    // 2. Let step be the navigable's current session history step.
    auto step = current_session_history_step();

    // 3. Let entryLists be the ordered set « navigable's session history entries ».
    Vector<Vector<JS::NonnullGCPtr<SessionHistoryEntry>>&> entry_lists;
    entry_lists.append(session_history_entries());

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto& entry_list = entry_lists.take_first();

        // 1. Remove every session history entry from entryList that has a step greater than step.
        entry_list.remove_all_matching([step](auto& entry) {
            return entry->step.template get<int>() > step;
        });

        // 2. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state->nested_histories()) {
                entry_lists.append(nested_history.entries);
            }
        }
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void TraversableNavigable::traverse_the_history_by_delta(int delta, Optional<DOM::Document&> source_document)
{
    // 1. Let sourceSnapshotParams and initiatorToCheck be null.
    Optional<SourceSnapshotParams> source_snapshot_params = {};
    JS::GCPtr<Navigable> initiator_to_check = nullptr;

    // 2. Let userInvolvement be "browser UI".
    UserNavigationInvolvement user_involvement = UserNavigationInvolvement::BrowserUI;

    // 1. If sourceDocument is given, then:
    if (source_document.has_value()) {
        // 1. Set sourceSnapshotParams to the result of snapshotting source snapshot params given sourceDocument.
        source_snapshot_params = source_document->snapshot_source_snapshot_params();

        // 2. Set initiatorToCheck to sourceDocument's node navigable.
        initiator_to_check = source_document->navigable();

        // 3. Set userInvolvement to "none".
        user_involvement = UserNavigationInvolvement::None;
    }

    // 4. Append the following session history traversal steps to traversable:
    append_session_history_traversal_steps([this, delta, source_snapshot_params = move(source_snapshot_params), initiator_to_check, user_involvement] {
        // 1. Let allSteps be the result of getting all used history steps for traversable.
        auto all_steps = get_all_used_history_steps();

        // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.
        auto current_step_index = *all_steps.find_first_index(current_session_history_step());

        // 3. Let targetStepIndex be currentStepIndex plus delta
        auto target_step_index = current_step_index + delta;

        // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
        if (target_step_index >= all_steps.size()) {
            return;
        }

        // 5. Apply the traverse history step allSteps[targetStepIndex] to traversable, given sourceSnapshotParams,
        //    initiatorToCheck, and userInvolvement.
        apply_the_traverse_history_step(all_steps[target_step_index], source_snapshot_params, initiator_to_check, user_involvement);
    });
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-for-navigable-creation/destruction
TraversableNavigable::HistoryStepResult TraversableNavigable::update_for_navigable_creation_or_destruction()
{
    // 1. Let step be traversable's current session history step.
    auto step = current_session_history_step();

    // 2. Return the result of applying the history step step to traversable given, false, null, null, and null.
    return apply_the_history_step(step, false, {}, {}, {});
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-reload-history-step
TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_reload_history_step()
{
    // 1. Let step be traversable's current session history step.
    auto step = current_session_history_step();

    // 2. Return the result of applying the history step step to traversable given true, null, null, and null.
    return apply_the_history_step(step, true, {}, {}, {});
}

TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_push_or_replace_history_step(int step)
{
    // 1. Return the result of applying the history step step to traversable given false, null, null, and null.
    return apply_the_history_step(step, false, {}, {}, {});
}

TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_traverse_history_step(int step, Optional<SourceSnapshotParams> source_snapshot_params, JS::GCPtr<Navigable> initiator_to_check, UserNavigationInvolvement user_involvement)
{
    // 1. Return the result of applying the history step step to traversable given true, sourceSnapshotParams, initiatorToCheck, and userInvolvement.
    return apply_the_history_step(step, true, move(source_snapshot_params), initiator_to_check, user_involvement);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#close-a-top-level-traversable
void TraversableNavigable::close_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let toUnload be traversable's active document's inclusive descendant navigables.
    auto to_unload = active_document()->inclusive_descendant_navigables();

    // FIXME: 2. If the result of checking if unloading is user-canceled for toUnload is true, then return.

    // 3. Unload the active documents of each of toUnload.
    for (auto navigable : to_unload) {
        navigable->active_document()->unload();
    }

    // 4. Destroy traversable.
    destroy_top_level_traversable();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
void TraversableNavigable::destroy_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let browsingContext be traversable's active browsing context.
    auto browsing_context = active_browsing_context();

    // 2. For each historyEntry in traversable's session history entries:
    for (auto& history_entry : m_session_history_entries) {
        // 1. Let document be historyEntry's document.
        auto document = history_entry->document_state->document();

        // 2. If document is not null, then destroy document.
        if (document)
            document->destroy();
    }

    // 3. Remove browsingContext.
    if (!browsing_context) {
        dbgln("TraversableNavigable::destroy_top_level_traversable: No browsing context?");
    } else {
        browsing_context->remove();
    }

    // 4. Remove traversable from the user interface (e.g., close or hide its tab in a tabbed browser).
    page().client().page_did_close_top_level_traversable();

    // 5. Remove traversable from the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().remove(this);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
void finalize_a_same_document_navigation(JS::NonnullGCPtr<TraversableNavigable> traversable, JS::NonnullGCPtr<Navigable> target_navigable, JS::NonnullGCPtr<SessionHistoryEntry> target_entry, JS::GCPtr<SessionHistoryEntry> entry_to_replace)
{
    // NOTE: This is not in the spec but we should not navigate destroyed navigable.
    if (target_navigable->has_been_destroyed())
        return;

    // FIXME: 1. Assert: this is running on traversable's session history traversal queue.

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    if (target_navigable->active_session_history_entry() != target_entry) {
        return;
    }

    // 3. Let targetStep be null.
    Optional<int> target_step;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.
    auto& target_entries = target_navigable->get_session_history_entries();

    // 5. If entryToReplace is null, then:
    if (!entry_to_replace) {
        // 1. Clear the forward session history of traversable.
        traversable->clear_the_forward_session_history();

        // 2. Set targetStep to traversable's current session history step + 1.
        target_step = traversable->current_session_history_step() + 1;

        // 3. Set targetEntry's step to targetStep.
        target_entry->step = *target_step;

        // 4. Append targetEntry to targetEntries.
        target_entries.append(target_entry);
    } else {
        // 1. Replace entryToReplace with targetEntry in targetEntries.
        *(target_entries.find(*entry_to_replace)) = target_entry;

        // 2. Set targetEntry's step to entryToReplace's step.
        target_entry->step = entry_to_replace->step;

        // 3. Set targetStep to traversable's current session history step.
        target_step = traversable->current_session_history_step();
    }

    // 6. Apply the push/replace history step targetStep to traversable.
    traversable->apply_the_push_or_replace_history_step(*target_step);
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-visibility-state
void TraversableNavigable::set_system_visibility_state(VisibilityState visibility_state)
{
    if (m_system_visibility_state == visibility_state)
        return;
    m_system_visibility_state = visibility_state;

    // When a user-agent determines that the system visibility state for
    // traversable navigable traversable has changed to newState, it must run the following steps:

    // 1. Let navigables be the inclusive descendant navigables of traversable's active document.
    auto navigables = active_document()->inclusive_descendant_navigables();

    // 2. For each navigable of navigables:
    for (auto& navigable : navigables) {
        // 1. Let document be navigable's active document.
        auto document = navigable->active_document();
        VERIFY(document);

        // 2. Queue a global task on the user interaction task source given document's relevant global object
        //    to update the visibility state of document with newState.
        queue_global_task(Task::Source::UserInteraction, relevant_global_object(*document), [visibility_state, document] {
            document->update_the_visibility_state(visibility_state);
        });
    }
}

}
