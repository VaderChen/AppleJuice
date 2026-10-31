#!/usr/bin/env python3
"""區分 Smoke、正常退出工作負載及有界 GUI 觀測，保留各自的退出契約。"""
import math


def bounded_launcher_row(row):
    """Launcher 以 UI 停止客體；其 -1 狀態不是客體自然退出碼。"""
    if (row.get('criterion') != 'menu_visible' or row.get('image_confirmed') is not True or
            row.get('stop_reason') != 'launcher_ui_stop' or row.get('launcher_exit_code') != 0 or
            row.get('program_state') != 'stopped' or row.get('program_exit_code') != -1 or
            row.get('natural_exit_code') is not None or 'natural_exit_code' not in row or
            row.get('all_producers_stopped') is not True or row.get('remaining_pids') != [] or
            row.get('guard_stop_reason') is not None or 'guard_stop_reason' not in row or
            row.get('guard_exit_code') != 0 or row.get('guard_child_exit_code') != 0 or
            row.get('backend') != 'jit' or row.get('launcher_ui_passed') is not True or
            any(row.get(key) is not True for key in ('engine_controls_applied', 'engine_unchanged',
                                                    'app_unchanged', 'source_settings_unchanged', 'group_stopped'))):
        raise ValueError('Launcher GUI 觀測未達選單、未完成 UI 停止或程序樹未收尾')
    checks = row.get('checks')
    if (not isinstance(checks, dict) or not checks or any(value is not True for value in checks.values()) or
            row.get('pressure_levels') != [1]):
        raise ValueError('Launcher GUI 觀測缺少通過的檢查或正常記憶體壓力')
    for key in ('image_sha256', 'report_sha256', 'exe_sha256', 'launcher_sha256'):
        digest = row.get(key)
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('Launcher GUI 觀測缺少輸入或畫面身分：' + key)


def bounded_gameplay_row(row):
    """以原生操作、唯讀狀態與 UI 收尾核對有界遊玩，不表示 FPS 或全部遊戲通過。"""
    if (row.get('observer') != 'launcher_ui' or row.get('criterion') != 'interactive_gameplay' or
            row.get('backend') != 'jit' or row.get('stop_reason') != 'launcher_ui_stop' or
            row.get('launcher_exit_code') != 0 or row.get('program_state') != 'stopped' or
            row.get('program_exit_code') != -1 or row.get('natural_exit_code') is not None or
            'natural_exit_code' not in row or row.get('all_producers_stopped') is not True or
            row.get('remaining_pids') != [] or row.get('guard_stop_reason') is not None or
            'guard_stop_reason' not in row or row.get('guard_exit_code') != 0 or
            row.get('pressure_levels') != [1] or
            any(row.get(key) is not True for key in ('engine_controls_applied', 'engine_unchanged',
                'app_unchanged', 'source_settings_unchanged', 'group_stopped', 'launcher_ui_passed',
                'readonly_diagnostics', 'all_inputs_released', 'directional_position_change'))):
        raise ValueError('有界遊玩缺少實際操作、正常守護或 UI 收尾契約')
    times = [row.get(key) for key in ('deadline_seconds', 'observation_seconds',
             'first_diagnostic_seconds', 'last_diagnostic_seconds', 'maximum_diagnostic_gap_seconds')]
    if any(isinstance(value, bool) or not isinstance(value, (int, float)) or
           not math.isfinite(value) for value in times):
        raise ValueError('有界遊玩診斷時間不完整')
    deadline, observed, first, last, gap = times
    if not (30 <= deadline <= observed and 0 <= first < last <= observed and
            first <= min(60, deadline/2) and last >= deadline-15 and 0 < gap <= 25):
        raise ValueError('唯讀狀態未涵蓋指定遊玩區間，或超過輸入器的新鮮度期限')
    cycles = row.get('repeat_cycle_observations', [])
    count = row.get('requested_repeat_cycles')
    if (row.get('input_status') != 'passed' or type(count) is not int or not 1 <= count <= 16 or
            len(cycles) != count or [cycle.get('cycle') for cycle in cycles] != list(range(1, count+1)) or
            any(type(cycle.get('steps')) is not int or cycle['steps'] <= 0 or
                type(cycle.get('position_changes')) is not int or cycle['position_changes'] <= 0
                for cycle in cycles)):
        raise ValueError('重複操作未完成，或沒有觀測到角色位移')
    if (type(row.get('tail_frame_counter_delta')) is not int or row['tail_frame_counter_delta'] <= 0 or
            any(type(row.get(key)) is not int or row[key] != 0
                for key in ('fault_context_count', 'heap_integrity_fault_count'))):
        raise ValueError('觀測末段未持續更新，或存在已記錄的客體故障')
    for key in ('report_sha256', 'input_sha256', 'diagnostic_sha256', 'exe_sha256', 'launcher_sha256'):
        digest = row.get(key)
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('有界遊玩缺少已核對的來源身分：' + key)


def bounded_gui_rows(evidence):
    rows = evidence.get('workloads', [])
    if evidence.get('smoke_run') is not False or evidence.get('backends') != ['jit'] or not rows:
        raise ValueError('缺少有界 GUI 實際觀測')
    for row in rows:
        if row.get('criterion') == 'interactive_gameplay':
            bounded_gameplay_row(row)
            continue
        times = [row.get(key) for key in ('image_seconds', 'deadline_seconds', 'observation_seconds')]
        if (any(isinstance(value, bool) or not isinstance(value, (int, float)) or
                not math.isfinite(value) for value in times) or not 0 <= times[0] <= times[1] <= times[2]):
            raise ValueError('GUI 影像不在指定觀測期限內')
        if row.get('observer') == 'launcher_ui':
            bounded_launcher_row(row)
            continue
        if (row.get('criterion') != 'menu_visible' or row.get('image_confirmed') is not True or
                row.get('nonfocused') is not True or row.get('stop_reason') != 'deadline' or
                row.get('returncode') != -15 or row.get('natural_exit_code') is not None or
                'natural_exit_code' not in row or row.get('all_producers_stopped') is not True or
                any(row.get(key) != [] for key in
                    ('remaining_pids', 'signal_errors', 'fatal_exceptions', 'module_errors')) or
                not row.get('backend')):
            raise ValueError('有界 GUI 觀測未達選單、未受控收尾或有執行錯誤')
        for key in ('image_sha256', 'report_sha256', 'exe_sha256', 'nw_dll_sha256', 'registry_sha256'):
            digest = row.get(key)
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
                raise ValueError('GUI 觀測缺少輸入或畫面身分：' + key)
    return rows


def from_evidence(evidence):
    if evidence.get('status') != 'passed':
        raise ValueError('引擎驗收未通過')
    if evidence.get('validation_kind') == 'bounded_gui_observation':
        rows = bounded_gui_rows(evidence)
        return dict(schema=3, kind='bounded_gui_observation', engine_sha256=evidence['engine_sha256'],
                    sdk_sha256=evidence['sdk_sha256'], backends=['jit'], workloads=rows,
                    smoke_run=False, scope=evidence['scope'])
    if evidence.get('validation_kind') == 'real_workload':
        rows = evidence.get('workloads', [])
        if (evidence.get('smoke_run') is not False or not rows or
                any(row.get('valid') != 1 or row.get('same_work') is not True or
                    row.get('exit_code') != 0 for row in rows) or
                evidence.get('backends') != ['jit']):
            raise ValueError('缺少實際工作負載的有效結果')
        return dict(schema=2, kind='real_workload', engine_sha256=evidence['engine_sha256'],
                    sdk_sha256=evidence['sdk_sha256'], backends=['jit'], workloads=rows,
                    smoke_run=False, scope=evidence['scope'])
    if set(evidence.get('backends', [])) != {'interpreter', 'jit'}:
        raise ValueError('既有 Smoke 缺少直譯器／JIT 結果')
    return dict(schema=1, engine_sha256=evidence['engine_sha256'],
                fixture_sha256=evidence['fixture_sha256'], backends=evidence['backends'], scope=evidence['scope'])


def from_state(state, engine_sha256):
    record = state.get('engine_validation', state.get('engine_smoke'))
    if not record or record['engine_sha256'] != engine_sha256:
        raise ValueError('驗收紀錄不屬於包內引擎')
    if record.get('schema') in (2, 3):
        kind = {2: 'real_workload', 3: 'bounded_gui_observation'}[record['schema']]
        if record.get('kind') != kind:
            raise ValueError('未知的引擎驗收種類')
        from_evidence(dict(record, status='passed', validation_kind=kind))
        sdks = [digest for path, digest in state['inputs']['files'].items() if path.endswith('/x64kernel/manifest.json')]
        if sdks != [record['sdk_sha256']]:
            raise ValueError('實測驗收與包內 SDK 身分不同')
    elif record.get('schema') != 1 or set(record.get('backends', [])) != {'interpreter', 'jit'}:
        raise ValueError('既有引擎驗收紀錄不完整')
    return record
