# --- BEGIN COPYRIGHT BLOCK ---
# Copyright (C) 2021 Red Hat, Inc.
# All rights reserved.
#
# License: GPL (version 3 or any later version).
# See LICENSE for details.
# --- END COPYRIGHT BLOCK ---
#
import time
import subprocess
import pytest
import logging
import os

from lib389 import DEFAULT_SUFFIX
from lib389.cli_idm.group import list, get, get_dn, create, delete, modify, rename
from lib389.topologies import topology_st
from lib389.cli_base import FakeArgs
from lib389.utils import ds_is_older, ensure_str
from lib389.idm.group import Groups
from . import check_value_in_log_and_reset

pytestmark = pytest.mark.tier0

logging.getLogger(__name__).setLevel(logging.DEBUG)
log = logging.getLogger(__name__)


@pytest.fixture(scope="function")
def create_test_group(topology_st, request):
    group_name = "test_group_2000"
    groups = Groups(topology_st.standalone, DEFAULT_SUFFIX)

    log.info('Create test group')
    if groups.exists(group_name):
        test_group = groups.get(group_name)
        test_group.delete()
    else:
        test_group = groups.create_test_group()

    def fin():
        log.info('Delete test group')
        if test_group.exists():
            test_group.delete()

    request.addfinalizer(fin)

""" Tests to create:
get rdn
get dn
rename keep old rdn
rename
"""


@pytest.mark.skipif(ds_is_older("1.4.2"), reason="Not implemented")
def test_dsidm_group_create(topology_st):
    """ Test dsidm group create option

    :id: 56e31cf5-0fbf-4693-b8d3-bc6f545d7734
    :setup: Standalone instance
    :steps:
        1. Run dsidm group create
        2. Check that a message is provided on creation
        3. Check that created group exists
    :expected results:
        1. Success
        2. Success
        3. Success
    """
    
    standalone = topology_st.standalone
    group_name = 'test_group'
    output = 'Successfully created {}'.format(group_name)

    args = FakeArgs()
    args.cn = group_name

    log.info('Test dsidm group create')
    create(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args)
    check_value_in_log_and_reset(topology_st, check_value=output)

    log.info('Check that group is present')
    groups = Groups(standalone, DEFAULT_SUFFIX)
    new_group = groups.get(group_name)
    assert new_group.exists()

    log.info('Clean up for next test')
    new_group.delete()


@pytest.mark.skipif(ds_is_older("1.4.2"), reason="Not implemented")
def test_dsidm_group_delete(topology_st, create_test_group):
    """ Test dsidm group delete option

    :id: d285db5d-8efd-4775-b5f4-47bf27106da6
    :setup: Standalone instance
    :steps:
        1. Run dsidm group delete on a created group
        2. Check that a message is provided on deletion
        3. Check that group does not exist
    :expected results:
        1. Success
        2. Success
        3. Success
    """

    standalone = topology_st.standalone
    groups = Groups(standalone, DEFAULT_SUFFIX)
    test_group = groups.get('test_group_2000')
    output = 'Successfully deleted {}'.format(test_group.dn)

    args = FakeArgs()
    args.dn = test_group.dn

    log.info('Test dsidm group delete')
    delete(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args, warn=False)
    check_value_in_log_and_reset(topology_st, check_value=output)

    log.info('Check that group does not exist')
    assert not test_group.exists()


@pytest.mark.skipif(ds_is_older("1.4.2"), reason="Not implemented")
def test_dsidm_group_modify(topology_st, create_test_group):
    """ Test dsidm group modify option

    :id: 43611907-b477-4793-9ed6-8cf2d43ddd6b
    :setup: Standalone instance
    :steps:
        1. Run dsidm group modify add description value
        2. Run dsidm group modify replace description value
        3, Run dsidm group modify delete description value
    :expected results:
        1. description value is present
        2. description value is replaced with the new one
        3. description value is deleted
    """

    standalone = topology_st.standalone
    groups = Groups(standalone, DEFAULT_SUFFIX)
    group_name = 'test_group_2000'
    test_group = groups.get(group_name)
    output = 'Successfully modified {}'.format(test_group.dn)

    args = FakeArgs()
    args.selector = group_name

    log.info('Test dsidm group modify add')
    args.changes = ['add:description:test']
    modify(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args, warn=False)
    check_value_in_log_and_reset(topology_st, check_value=output)
    assert test_group.present('description', 'test')
    
    log.info('Test dsidm group modify replace')
    args.changes = ['replace:description:replaced']
    modify(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args, warn=False)
    check_value_in_log_and_reset(topology_st, check_value=output)
    assert test_group.present('description', 'replaced')
    
    log.info('Test dsidm group modify delete')
    args.changes = ['delete:description:replaced']
    modify(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args, warn=False)
    check_value_in_log_and_reset(topology_st, check_value=output)
    assert not test_group.present('description', 'replaced')


@pytest.mark.skipif(ds_is_older("1.4.2"), reason="Not implemented")
def test_dsidm_group_list(topology_st, create_test_group):
    """ Test dsidm group list option

    :id: e10c9430-1ea5-4b81-98c0-524f4f7da0e6
    :setup: Standalone instance
    :steps:
        1. Run dsidm group list option without json
        2. Check the output content is correct
        3. Run dsidm group list option with json
        4. Check the output content is correct
        5. Delete the group
        6. Check the group is not in the list with json
        7. Check the group is not in the list without json
    :expected results:
        1. Success
        2. Success
        3. Success
        4. Success
        5. Success
        6. Success
        7. Success
    """

    standalone = topology_st.standalone
    args = FakeArgs()
    args.json = False
    group_value = 'test_group_2000'
    json_list = ['type',
                 'list',
                 'items']

    log.info('Empty the log file to prevent false data to check about group')
    topology_st.logcap.flush()

    log.info('Test dsidm group list without json')
    list(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args)
    check_value_in_log_and_reset(topology_st, check_value=group_value)

    log.info('Test dsidm group list with json')
    args.json = True
    list(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args)
    check_value_in_log_and_reset(topology_st, content_list=json_list, check_value=group_value)

    log.info('Delete the group')
    groups = Groups(standalone, DEFAULT_SUFFIX)
    testgroup = groups.get(group_value)
    testgroup.delete()

    log.info('Test empty dsidm group list with json')
    list(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args)
    check_value_in_log_and_reset(topology_st, content_list=json_list, check_value_not=group_value)

    log.info('Test empty dsidm group list without json')
    args.json = False
    list(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args)
    check_value_in_log_and_reset(topology_st, check_value_not=group_value)


if __name__ == '__main__':
    # Run isolated
    # -s for DEBUG mode
    CURRENT_FILE = os.path.realpath(__file__)
    pytest.main("-s %s" % CURRENT_FILE)
