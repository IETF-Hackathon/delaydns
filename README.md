`deleydns`
==========

Overview
--------

`delaydns` is a DNS proxy that adds delay to DNS replies.
This is useful for testing Out Of Order Processing support with name servers
that offer pipelining of queries in "kept open" stateful transports.
To test the Out Of Order Processing capability with such name servers, you
query for a *delayed* name first, followed by a few normal domain names.

The delay added to a reply by `delaydns` is read from the first label of
the query.  The delay added is the number of miliseconds specified by the
number made up by digits making up the start of the first label of the query.

We have a `delaydns` live proxying for `delay.getdnsapi.net`.

For example to query for a name adding a 2 second delay:

    dig 2000.delay.getdnsapi.net


Prerequisites
-------------

  * GNU autotools
  * The getdns library


Building
--------

    autoreconf -vfi
    ./configure && make && make install

Running
-------

    delaydns [ @<target ns IP> ... ] <listening IP>
