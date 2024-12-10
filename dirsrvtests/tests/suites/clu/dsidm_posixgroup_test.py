# --- BEGIN COPYRIGHT BLOCK ---
# Copyright (C) 2024 Red Hat, Inc.
# All rights reserved.
#
# License: GPL (version 3 or any later version).
# See LICENSE for details.
# --- END COPYRIGHT BLOCK ---

import pytest
import logging
import os

from lib389 import DEFAULT_SUFFIX
from lib389.cli_idm.posixgroup import list, get, get_dn, create, delete, modify, rename
from lib389.topologies import topology_st
from lib389.cli_base import FakeArgs
from lib389.utils import ds_is_older, ensure_str
from lib389.idm.posixgroup import PosixGroups
from . import check_value_in_log_and_reset

pytestmark = pytest.mark.tier0

logging.getLogger(__name__).setLevel(logging.DEBUG)
log = logging.getLogger(__name__)


posixgroup_name = 'test_posixgroup'

@pytest.fixture(scope="function")
def create_test_posixgroup(topology_st, request):
    posixgroups = PosixGroups(topology_st.standalone, DEFAULT_SUFFIX)

    log.info('Create test posixgroup')
    if posixgroups.exists(posixgroup_name):
        test_posixgroup = posixgroups.get(posixgroup_name)
        test_posixgroup.delete()

    properties = FakeArgs()
    properties.cn = posixgroup_name
    properties.gidNumber = '3000'
    create(topology_st.standalone, DEFAULT_SUFFIX, topology_st.logcap.log, properties)
    test_posixgroup = posixgroups.get(posixgroup_name)

    def fin():
        log.info('Delete test posixgroup')
        if test_posixgroup.exists():
            test_posixgroup.delete()

    request.addfinalizer(fin)


@pytest.mark.skipif(ds_is_older("1.4.2"), reason="Not implemented")
def test_dsidm_posixgroup_create(topology_st):
    """ Test dsidm posixgroup create option

    :id:
    :setup: Standalone instance
    :steps:
        1. Run dsidm posixgroup create
        2. Check that a message is provided on creation
        3, Check that created posixgroup exists
    :expectedresults:
        1. Success
        2. Success
        3. Success
    """

    standalone = topology_st.standalone
    output = 'Successfully created {}'.format(posixgroup_name)

    args = FakeArgs()
    args.cn = posixgroup_name
    args.gidNumber = '3000'

    log.info('Test dsidm posixgroup create')
    create(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args)
    check_value_in_log_and_reset(topology_st, check_value=output)

    log.info('Check that posixgroup is present')
    posixgroups = PosixGroups(standalone, DEFAULT_SUFFIX)
    new_posixgroup = posixgroups.get(posixgroup_name)
    assert new_posixgroup.exists()

    log.info('Clean up for next test')
    new_posixgroup.delete()


@pytest.mark.skipif(ds_is_older("1.4.2"), reason="Not implemented")
def test_dsidm_posixgroup_delete(topology_st, create_test_posixgroup):
    """ Test dsidm posixgroup delete option

    :id:
    :setup: Standalone instance
    :steps:
        1. Run dsidm posixgroup delete on a created group
        2. Check that a message is provided on deletion
        3. Check that posixgroup does not exist
    :expectedresults:
        1. Success
        2. Success
        3. Success
    """

    standalone = topology_st.standalone
    posixgroups = PosixGroups(standalone, DEFAULT_SUFFIX)
    test_posixgroup = posixgroups.get(posixgroup_name)
    output = 'Successfully deleted {}'.format(test_posixgroup.dn)

    args = FakeArgs()
    args.dn = test_posixgroup.dn

    log.info('Test dsidm posixgroup delete')
    delete(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args, warn=False)
    check_value_in_log_and_reset(topology_st, check_value=output)

    log.info('Check that posixgroup does not exist')
    assert not test_posixgroup.exists()