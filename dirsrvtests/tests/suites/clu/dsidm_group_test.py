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


@pytest.mark.skipif(ds_is_older("1.4.2"), reason="Not implemented")
def test_dsidm_group_list(topology_st, create_test_group):
    """ Test dsidm group list option

    :id:
    :setup: Standalone instance
    :steps:
    :expected results:
    """

    standalone = topology_st.standalone
    args = FakeArgs()
    group_value = 'test_group_2000'

    log.info('Empty the log file to prevent false data to check about group')
    topology_st.logcap.flush()

    log.info('Test dsidm group user list')
    list(standalone, DEFAULT_SUFFIX, topology_st.logcap.log, args)
    check_value_in_log_and_reset(topology_st, check_value=group_value)

if __name__ == '__main__':
    # Run isolated
    # -s for DEBUG mode
    CURRENT_FILE = os.path.realpath(__file__)
    pytest.main("-s %s" % CURRENT_FILE)
