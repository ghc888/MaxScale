# Upgrading MaxScale from 1.3 to 1.4

This document describes upgrading MaxScale from version 1.3 to 1.4.

For more detailed information, please refer to the [release notes](../Release-Notes/MaxScale-1.4.0-Release-Notes.md).

## Installation

Before starting the upgrade, we **strongly** recommend you back up your current
configuration file.

## Password encryption

MaxScale 1.4.0 upgrades the used password encryption algorithms to more secure ones.
This requires that the password files are recreated with the `maxkeys` tool.
For more information about how to do this, please read the installation guide:
[MariaDB MaxScale Installation Guide](../Documentation/Getting-Started/MariaDB-MaxScale-Installation-Guide.md)

## SSL

The SSL configuration parameters are now a part of the listeners. If a service
used the old style SSL configuration parameters, the values should be moved to
the listener which is associated with that service.

Here is an example of an old style configuration.

```
[RW Split Router]
type=service
router=readwritesplit
servers=server1,server2,server3,server4
user=jdoe
passwd=BD26E4139A15280CA882264AA1551C70
ssl=required
ssl_cert=/home/user/certs/server-cert.pem
ssl_key=/home/user/certs/server-key.pem
ssl_ca_cert=/home/user/certs/ca.pem
ssl_version=TLSv12

[RW Split Listener]
type=listener
service=RW Split Router
protocol=MySQLClient
port=3306
```

And here is the new, 1.4.0 compatible configuration style.

```
[RW Split Router]
type=service
router=readwritesplit
servers=server1,server2,server3,server4
user=jdoe
passwd=BD26E4139A15280CA882264AA1551C70

[RW Split Listener]
type=listener
service=RW Split Router
protocol=MySQLClient
port=3306
ssl=required
ssl_cert=/home/user/certs/server-cert.pem
ssl_key=/home/user/certs/server-key.pem
ssl_ca_cert=/home/user/certs/ca.pem
ssl_version=TLSv12
```

Please also note that the `enabled` SSL mode is no longer supported due to
the inherent security issues with allowing SSL and non-SSL connections on
the same port. In addition to this, SSLv3 is no longer supported due to
vulnerabilities found in it.
