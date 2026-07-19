const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const engine = fs.readFileSync(path.join(root, 'core', 'mutation_engine.cpp'), 'utf8');
const loader = fs.readFileSync(path.join(root, 'core', 'rule_package.cpp'), 'utf8');
const embedded = fs.readFileSync(path.join(root, 'tests', 'mutation_pipeline_embedded.cpp'), 'utf8');
const main = fs.readFileSync(path.join(root, 'main.cpp'), 'utf8');

function section(source, begin, end) {
    const start = source.indexOf(begin);
    const finish = source.indexOf(end, start + begin.length);
    assert(start >= 0, `missing section start: ${begin}`);
    assert(finish > start, `missing section end: ${end}`);
    return source.slice(start, finish);
}

const stageRule = section(
    engine,
    'bool MutationEngine::stageRule(',
    'bool MutationEngine::stageMutation(');
assert(!stageRule.includes('runtime_state_'), 'stageRule must not mutate live runtime state');
assert(
    stageRule.includes('publish_runtime_on_commit = true'),
    'stageRule must mark pending runtime state for commit');
assert(stageRule.includes('advanceRuleEpoch()'),
    'every staged replacement must invalidate handles for reusable slots');

const commit = section(
    engine,
    'bool MutationEngine::applyCommit()',
    'size_t MutationEngine::stagingCount()');
const compile = commit.indexOf('if (!compileRule(');
const pause = commit.indexOf('pauseMutationReads()');
const runtimePublish = commit.indexOf('runtime_state_[i].current_value.store');
const tableSwap = commit.indexOf('swapActiveTable()');
const resume = commit.indexOf('resumeMutationReads()');
assert(
    compile >= 0 && compile < pause && pause < runtimePublish && runtimePublish < tableSwap && tableSwap < resume,
    'commit must compile, quiesce readers, publish runtime, swap the table, then resume readers');
assert(
    !commit.slice(0, pause).includes('runtime_state_['),
    'fallible commit work must not touch live runtime state');

const applyFrame = section(
    engine,
    'size_t MutationEngine::applyFrameMutations(',
    'int32_t MutationEngine::registerDynamicSignalRule(');
assert(applyFrame.includes('if (!beginMutationRead())'), 'frame mutation must use the nonblocking reader gate');
assert(applyFrame.includes('MutationReadScope read_scope'), 'frame mutation must scope its reader slot');

const hasRules = section(
    engine,
    'bool MutationEngine::hasRulesForFrame(',
    'size_t MutationEngine::applyFrameMutations(');
assert(hasRules.includes('if (!beginMutationRead())'), 'rule lookup must use the nonblocking reader gate');
assert(hasRules.includes('MutationReadScope read_scope'), 'rule lookup must scope its reader slot');

const clearRules = section(
    engine,
    'void MutationEngine::clearRules()',
    'size_t MutationEngine::listRules(');
assert(!clearRules.includes('runtime_state_'), 'clearRules must not alter live runtime before commit');
assert(clearRules.includes('applyCommit()'), 'clearRules must publish an empty table through the commit barrier');

const clearStaging = section(
    engine,
    'void MutationEngine::clearStaging()',
    'void MutationEngine::revertStagingToActive()');
assert(clearStaging.includes('advanceRuleEpoch()'),
    'discarding staging must invalidate abandoned pending handles');

const setRuleValue = section(
    engine,
    'bool MutationEngine::setRuleValue(',
    'bool MutationEngine::enableRule(');
assert(setRuleValue.includes('if (rule_id >= kMaxRules || !beginMutationRead())'),
    'value control must enter the commit reader handshake');
assert(setRuleValue.includes('MutationReadScope read_scope'),
    'value control validation and write must share one scoped read token');
assert(setRuleValue.includes('expected_epoch != ruleEpoch()'),
    'value control must reject stale rule epochs');
assert(setRuleValue.includes('ruleExistsForControl(rule_id)'),
    'value control must reject nonexistent rule slots');

const enableRule = section(
    engine,
    'bool MutationEngine::enableRule(',
    'void MutationEngine::clearRules()');
assert(enableRule.includes('if (rule_id >= kMaxRules || !beginMutationRead())'),
    'enable control must enter the commit reader handshake');
assert(enableRule.includes('MutationReadScope read_scope'),
    'enable validation and write must share one scoped read token');
assert(enableRule.includes('expected_epoch != ruleEpoch()'),
    'enable control must reject stale rule epochs');
assert(enableRule.includes('ruleExistsForControl(rule_id)'),
    'enable control must reject nonexistent rule slots');

const listRules = section(
    engine,
    'size_t MutationEngine::listRules(',
    'uint32_t MutationEngine::keyHash(');
assert(listRules.includes('if (!beginMutationRead())'), 'rule listing must use the nonblocking reader gate');
assert(listRules.includes('MutationReadScope read_scope'), 'rule listing must scope its reader slot');

const stagedList = section(
    engine,
    'size_t MutationEngine::listStagedRules(',
    'uint32_t MutationEngine::keyHash(');
assert(stagedList.includes('sequence > staged_'),
    'candidate listing must preserve staging sequence instead of slot order');
assert(stagedList.includes('dst.request.enabled = src.pending_enabled'),
    'candidate listing must expose pending enabled state');
assert(stagedList.includes('src.pending_runtime_value'),
    'candidate listing must expose pending dynamic values');

const loaderEntry = loader.indexOf('bool RulePackageLoader::loadCsv(');
const loaderGuard = loader.indexOf('StagingFailureGuard staging_guard(engine)', loaderEntry);
const inputReject = loader.indexOf('if (text == nullptr || length == 0U) return false', loaderEntry);
const guardRelease = loader.indexOf('staging_guard.release()', loaderEntry);
assert(
    loaderEntry >= 0 && loaderGuard > loaderEntry && loaderGuard < inputReject && guardRelease > inputReject,
    'rule-package cleanup guard must cover every loader failure exit');

for (const marker of [
    'testStagedCandidateListing',
    'candidate list exposes pending enabled state',
    'candidate rule cannot mutate frames before apply',
    'testFailedPackagesAreTransactional',
    'BIND_ACTIVE,profileCode,16',
    'missingRuntime',
    'testStagedRuntimePublishesOnlyOnCommit',
    'testRuleHandleEpochRejectsSlotReuse',
    'abandoned pending value cannot alter a reused staged slot',
    'stale enable cannot activate a reused rule slot',
    'testClearRulesCommitsEmptyTable',
]) {
    assert(embedded.includes(marker), `missing embedded transaction regression: ${marker}`);
}

const valueStart = main.lastIndexOf('void handleRuleValue()');
const enableStart = main.lastIndexOf('void handleRuleEnable()');
const replayStart = main.lastIndexOf('void handleReplayLoad()');
assert(valueStart >= 0 && enableStart > valueStart && replayStart > enableStart,
    'missing concrete rule-control HTTP handlers');
const valueHandler = main.slice(valueStart, enableStart);
assert(valueHandler.includes('parseUIntArg("rule_epoch"'),
    'explicit rule value writes must require the epoch-qualified handle');
assert(valueHandler.includes('stale_rule_handle'),
    'stale value handles must fail closed at the HTTP boundary');

const enableHandler = main.slice(enableStart, replayStart);
assert(enableHandler.includes('parseUIntArg("rule_epoch"'),
    'explicit rule enable writes must require the epoch-qualified handle');
assert(enableHandler.includes('stale_rule_handle'),
    'stale enable handles must fail closed at the HTTP boundary');

const listStart = main.lastIndexOf('void handleRulesList()');
assert(listStart >= 0 && valueStart > listStart, 'missing concrete rule-list HTTP handler');
const ruleListHandler = main.slice(listStart, valueStart);
assert(ruleListHandler.includes('rule_epoch'),
    'rule listing must expose the epoch paired with every reusable rule ID');

const stageStart = main.lastIndexOf('void handleRuleStage()');
const actionStart = main.lastIndexOf('void handleRulesAction()');
assert(stageStart >= 0 && actionStart > stageStart && listStart > actionStart,
    'missing staged/structural rule HTTP handlers');
const stageHandler = main.slice(stageStart, actionStart);
assert(stageHandler.includes('ApplicationLockGuard app_lock'),
    'generic staging must serialize with application package loading');
const actionHandler = main.slice(actionStart, listStart);
assert(actionHandler.includes('active_rule_package_path = ""'),
    'a generic structural commit must stop claiming the official package is active');
assert(actionHandler.includes('invalidateApplicationResourcesLocked()'),
    'generic structural replacement must withdraw application resource validation');

console.log('mutation transaction source contract passed');
