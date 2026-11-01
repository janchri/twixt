Link
====

The link and ``twixt,i2c-mm-gpio`` on a fake bus: the driver's own code, with fake pins and a fake clock. Electrical timing still needs real boards.

Native
------

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/tests/link

Native only.
