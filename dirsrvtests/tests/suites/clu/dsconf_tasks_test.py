# --- BEGIN COPYRIGHT BLOCK ---
# Copyright (C) 2023 Red Hat, Inc.
# All rights reserved.
#
# License: GPL (version 3 or any later version).
# See LICENSE for details.
# --- END COPYRIGHT BLOCK ---
#
import os
import subprocess
import uuid

import pytest

from lib389._constants import DEFAULT_SUFFIX, DN_DM
from test389.topologies import topology_st as topo
from lib389.tasks import (ImportTask, ExportTask, AutomemberRebuildMembershipTask, MemberOfFixupTask,
                          USNTombstoneCleanupTask, EntryUUIDFixupTask, SyntaxValidateTask)
from lib389.plugins import USNPlugin, POSIXWinsyncPlugin, LinkedAttributesPlugin, AutoMembershipPlugin, MemberOfPlugin
from lib389.dbgen import dbgen_users
from lib389.idm.user import UserAccount
from lib389.idm.group import Groups

DSCONF = '/usr/sbin/dsconf'
# Enough entries for import/export/reindex to emit multiple nsTaskLog updates when --watch is used.
WATCH_TEST_NUM_USERS = 20000


def _dsconf_base_cmd(inst):
    return [DSCONF, inst.serverid, '-D', DN_DM, '-w', 'password']


def _run_dsconf(inst, argv, timeout=7200):
    """Run dsconf; return (returncode, combined stdout+stderr)."""
    cmd = _dsconf_base_cmd(inst) + argv
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    out = (proc.stdout or '') + (proc.stderr or '')
    return proc.returncode, out


def _non_empty_lines(text):
    return [ln for ln in text.splitlines() if ln.strip()]


def _assert_watch_output(inst, no_watch_argv, watch_argv, progress_message, task_name):
    """Verify that a task emits additional progress output with ``--watch``."""
    rc, no_watch_output = _run_dsconf(inst, no_watch_argv)
    assert rc == 0, no_watch_output
    no_watch_lines = len(_non_empty_lines(no_watch_output))
    assert progress_message not in no_watch_output
    rc, watch_output = _run_dsconf(inst, watch_argv)
    assert rc == 0, watch_output
    assert len(_non_empty_lines(watch_output)) > no_watch_lines, task_name
    assert progress_message in watch_output


def _wait_for_task_timeout(task, timeout=.5):
    """Verify that a task is still running at the specified timeout."""
    task.wait(timeout=timeout, sleep_interval=timeout)
    assert task.get_exit_code() is None
    task.wait(timeout=0)


def _import_ldif(inst, ldif_file):
    """Import an LDIF and wait for completion."""
    task = ImportTask(inst)
    task.import_suffix_from_ldif(ldiffile=ldif_file, suffix=DEFAULT_SUFFIX)
    task.wait(timeout=0)
    assert task.get_exit_code() == 0


@pytest.fixture(scope='module')
def watch_test_data(topo):
    inst = topo.standalone
    import_ldif = os.path.join(inst.ldifdir, 'dsconf_watch_test_import.ldif')
    dbgen_users(inst, WATCH_TEST_NUM_USERS, import_ldif, DEFAULT_SUFFIX,
                parent='ou=people,' + DEFAULT_SUFFIX, generic=True)
    return inst, import_ldif


@pytest.fixture(scope='module')
def populated_watch_test_data(watch_test_data):
    inst, import_ldif = watch_test_data
    if not UserAccount(inst, dn=f'uid=user000001,ou=people,{DEFAULT_SUFFIX}').exists():
        _import_ldif(inst, import_ldif)
    return inst


def test_dsconf_import_watch_output(watch_test_data):
    """Import --watch reports progress

    :id: faf879f7-90f0-45bf-aabf-37ed96c52021
    :setup: Standalone Instance and a large LDIF
    :steps:
        1. Import without --watch
        2. Import with --watch
    :expectedresults:
        1. The import succeeds without progress output
        2. The watched import reports progress
    """
    inst, import_ldif = watch_test_data
    _assert_watch_output(inst, ['backend', 'import', 'userRoot', import_ldif],
                         ['backend', 'import', 'userRoot', import_ldif, '--watch'], 'Processing file', 'import')


def test_dsconf_export_watch_output(populated_watch_test_data):
    """Export --watch reports progress

    :id: e6b14acc-6952-403c-8fc4-62230bee8afa
    :setup: Standalone Instance with a populated backend
    :steps:
        1. Export without --watch
        2. Export with --watch
    :expectedresults:
        1. The export succeeds without progress output
        2. The watched export reports progress
    """
    inst = populated_watch_test_data
    _assert_watch_output(inst, ['backend', 'export', 'userRoot', '-l', os.path.join(inst.ldifdir, 'watch_export_a.ldif')],
                         ['backend', 'export', 'userRoot', '-l', os.path.join(inst.ldifdir, 'watch_export_b.ldif'), '--watch'],
                         'userRoot: Processed', 'export')


def test_dsconf_reindex_watch_output(populated_watch_test_data):
    """Reindex --watch reports progress

    :id: 64015ec5-3dea-4a12-a65d-a421a7fe28c8
    :setup: Standalone Instance with a populated backend
    :steps:
        1. Reindex with --wait
        2. Reindex with --wait --watch
    :expectedresults:
        1. The reindex succeeds without progress output
        2. The watched reindex reports progress
    """
    _assert_watch_output(populated_watch_test_data, ['backend', 'index', 'reindex', 'userRoot', '--wait'],
                         ['backend', 'index', 'reindex', 'userRoot', '--wait', '--watch'], 'Indexing attribute:', 'reindex')


@pytest.mark.parametrize('operation', ['create', 'restore'])
def test_dsconf_backup_watch_output(populated_watch_test_data, operation):
    """Backup operations with --watch report progress

    :id: 9fe71c15-59ff-4878-8275-e2bf70a03237
    :parametrized: yes
    :setup: Standalone Instance with a populated backend
    :steps:
        1. Run a backup operation without --watch
        2. Run the backup operation with --watch
    :expectedresults:
        1. The operation succeeds without progress output
        2. The watched operation reports progress
    """
    inst = populated_watch_test_data
    tag = uuid.uuid4().hex[:12]
    if operation == 'create':
        no_watch = os.path.join(inst.ds_paths.backup_dir, f'watch_create_a_{tag}')
        watch = os.path.join(inst.ds_paths.backup_dir, f'watch_create_b_{tag}')
        _assert_watch_output(inst, ['backup', 'create', no_watch], ['backup', 'create', watch, '--watch'],
                             'Creating backup', 'backup create')
    else:
        no_watch = os.path.join(inst.ds_paths.backup_dir, f'watch_restore_a_{tag}')
        watch = os.path.join(inst.ds_paths.backup_dir, f'watch_restore_b_{tag}')
        for backup_dir in (no_watch, watch):
            rc, output = _run_dsconf(inst, ['backup', 'create', backup_dir])
            assert rc == 0, output
        _assert_watch_output(inst, ['backup', 'restore', no_watch, '--timeout', '0'],
                             ['backup', 'restore', watch, '--timeout', '0', '--watch'],
                             'Restoring backup', 'backup restore')


@pytest.fixture(scope='module')
def timeout_task_instance(topo):
    inst = topo.standalone
    for plugin in [USNPlugin, POSIXWinsyncPlugin, LinkedAttributesPlugin, AutoMembershipPlugin, MemberOfPlugin]:
        plugin(inst).enable()
    inst.restart()
    return inst


@pytest.fixture(scope='module')
def timeout_import_ldif(timeout_task_instance):
    path = os.path.join(timeout_task_instance.ldifdir, 'import_task_timeout.ldif')
    dbgen_users(timeout_task_instance, 100000, path, DEFAULT_SUFFIX,
                parent='ou=people,' + DEFAULT_SUFFIX, generic=True, entry_name='timeout_user')
    return path


@pytest.fixture(scope='module')
def populated_timeout_database(timeout_task_instance, timeout_import_ldif):
    if not UserAccount(timeout_task_instance, dn=f'uid=timeout_user100000,ou=people,{DEFAULT_SUFFIX}').exists():
        _import_ldif(timeout_task_instance, timeout_import_ldif)
    return timeout_task_instance


def test_import_task_timeout(timeout_task_instance, timeout_import_ldif):
    """Import task honors a short wait timeout

    :id: 7a0b88eb-19cb-41d1-93b2-f5a8682f9a4b
    :setup: Standalone Instance and a large LDIF
    :steps:
        1. Start an import task
        2. Wait briefly and then for completion
    :expectedresults:
        1. The import task starts
        2. It remains running after the short wait and completes
    """
    task = ImportTask(timeout_task_instance)
    task.import_suffix_from_ldif(ldiffile=timeout_import_ldif, suffix=DEFAULT_SUFFIX)
    _wait_for_task_timeout(task)


def test_export_task_timeout(populated_timeout_database):
    """Export task handles a short wait timeout

    :id: e6b580bb-167d-4e79-8785-5ff1f8ecd82d
    :setup: Standalone Instance with a populated backend
    :steps:
        1. Start an export task
        2. Wait briefly and then for completion
    :expectedresults:
        1. The export task starts
        2. It either completes early or completes after the wait
    """
    task = ExportTask(populated_timeout_database)
    task.export_suffix_to_ldif(os.path.join(populated_timeout_database.ldifdir, 'export_task_timeout.ldif'), DEFAULT_SUFFIX)
    task.wait(timeout=.5, sleep_interval=.5)
    if task.get_exit_code() is None:
        task.wait(timeout=0)
    else:
        assert task.get_exit_code() == 0


@pytest.mark.parametrize('task_class, timeout', [(SyntaxValidateTask, .5), (AutomemberRebuildMembershipTask, .2),
                                                  (EntryUUIDFixupTask, .5)],
                         ids=['syntax_validate', 'automember_rebuild', 'entryuuid_fixup'])
def test_maintenance_task_timeout(populated_timeout_database, task_class, timeout):
    """Maintenance tasks honor a short wait timeout

    :id: e4c6a12e-5dda-47f7-b612-2bce69f7636d
    :parametrized: yes
    :setup: Standalone Instance with a populated backend
    :steps:
        1. Start a maintenance task
        2. Wait briefly and then for completion
    :expectedresults:
        1. The task starts
        2. It remains running after the short wait and completes
    """
    task = task_class(populated_timeout_database).create(properties={'basedn': DEFAULT_SUFFIX, 'filter': 'objectClass=*'})
    _wait_for_task_timeout(task, timeout)


def test_usn_tombstone_cleanup_task_timeout(populated_timeout_database):
    """USN tombstone cleanup honors a short wait timeout

    :id: d9279df8-4b14-4f4e-b91b-fb42cff4be97
    :setup: Standalone Instance with deleted user entries
    :steps:
        1. Delete user entries
        2. Start tombstone cleanup and wait briefly
    :expectedresults:
        1. Tombstones are created
        2. The task remains running after the short wait and completes
    """
    for idx in range(1, 2001):
        UserAccount(populated_timeout_database, dn=f'uid=timeout_user{str(idx).zfill(6)},ou=people,{DEFAULT_SUFFIX}').delete()
    _wait_for_task_timeout(USNTombstoneCleanupTask(populated_timeout_database).create(properties={'suffix': DEFAULT_SUFFIX}))


def test_memberof_fixup_task_timeout(populated_timeout_database):
    """MemberOf fixup honors a short wait timeout

    :id: 00d257a5-66d1-479d-94c2-d9d00aef278a
    :setup: Standalone Instance with a large group membership
    :steps:
        1. Add many users to a group
        2. Start MemberOf fixup and wait briefly
    :expectedresults:
        1. The group contains the users
        2. The task remains running after the short wait and completes
    """
    group = Groups(populated_timeout_database, DEFAULT_SUFFIX).create(properties={'cn': 'test_group'})
    for idx in range(5000, 6000):
        group.add_member(f'uid=timeout_user{str(idx).zfill(6)},ou=people,{DEFAULT_SUFFIX}')
    task = MemberOfFixupTask(populated_timeout_database).create(properties={'basedn': DEFAULT_SUFFIX, 'filter': 'objectClass=*'})
    _wait_for_task_timeout(task)


if __name__ == '__main__':
    # Run isolated
    # -s for DEBUG mode
    CURRENT_FILE = os.path.realpath(__file__)
    pytest.main(["-s", CURRENT_FILE])
