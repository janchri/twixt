SMP gateway
===========

The core of the MCUmgr gateway, with the service replaced by fakes: resends, late answers, no answer, freed packets, registered front ends. The serial and Bluetooth front ends are covered by ``tests/smp_update``.

Native
------

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/tests/smp_gateway

Native only.
