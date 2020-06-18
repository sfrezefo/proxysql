
To address Azure Database for MySQL connecyion specificity We have defined a specific behaviour for proxysql.With Azure Database for MySQL the username syntax include the database name. 
<pre lang="sql" cssfile="another_style" >
mysql -h mysqlpaasmaster.mysql.database.azure.com -u sbtest@mysqlpaasmaster -p \
  --ssl-mode=REQUIRED

We connect to the master and create 2 users : 'sbtest' for injecting traffic and 'monitoruser' required by proxysql to monitor the backend servers :
 

This means we have a different user for each instances. This  does not fit the ProxySQL connection to backends pattern. We will a common user for all backends. ProxySQL will transparently at connection time inject the database name in the username. 

For example for a username defined as <strong>'sbtest'</strong> it will generate different usernames to be used to connect to different backends
<pre lang="bash" cssfile="another_style" >
sbtest@mysqlpaasmaster if connecting to mysqlpaasmaster.mysql.database.azure.com
sbtest@mysqlpaasreplica1 if connecting to mysqlpaasreplica1.mysql.database.azure.com
sbtest@mysqlpaasreplica2 if connecting to mysqlpaasreplica2.mysql.database.azure.com
</pre>

it will do the same when connecting to the monitoring user <strong>'monitoruser'</strong>
<pre lang="bash" cssfile="another_style" >
monitoruser@mysqlpaasmaster if connecting to mysqlpaasmaster.mysql.database.azure.com
monitoruser@mysqlpaasreplica1 if connecting to mysqlpaasreplica1.mysql.database.azure.com
monitoruser@mysqlpaasreplica2 if connecting to mysqlpaasreplica2.mysql.database.azure.com
</pre>

you can set a global variable that will automatically the 

To activate the Azure specific behavior when using Azure Database for MySQL / MariaDB:
<pre lang="bash" cssfile="another_style" >
set mysql-azure_gen1_username='true';
load mysql variables to runtime; ONLINE
</pre>
This is manadatory to use proxySQL with Azure Database for MySQL 




CREATE SCHEMA sbtest;
CREATE USER sbtest@'%' IDENTIFIED BY 'Passw0rd';
GRANT ALL PRIVILEGES ON sbtest.* to sbtest@'%';

CREATE USER 'monitoruser'@'%' IDENTIFIED BY 'Passw0rd'; 
GRANT SELECT ON *.* TO 'monitoruser'@'%' WITH GRANT OPTION; 
FLUSH PRIVILEGES; 
</pre>
Now we configure proxysql through the proxysql admin. At initial startup proxysql reads its configuration from /etc/proxysql.cnf. This is where the admin user credentials are defined :
<pre lang="bash" cssfile="another_style" >
        admin_credentials="proxysqladmin:Passw0rd"
        mysql_ifaces="0.0.0.0:6032"
</pre>
All the rest of the configuration can be done in a scripted way that will be persisted to disk in a SQLite database.
<pre lang="bash" cssfile="another_style" >
mysql -h 127.0.0.1  -u proxysqladmin -pPassw0rd -P6032 --ssl

set mysql-monitor_username='monitoruser'; 
set mysql-monitor_password='Passw0rd'; 
</pre>

Before defining the servers we activate the azure feature so that they can be reached by proxysql monitoring through a common monitoring user. 
<pre lang="bash" cssfile="another_style" >
set mysql-azure_gen1_username='true';

insert into mysql_servers(hostgroup_id,hostname,port,weight,use_ssl, comment) 
  values(10,'mysqlpaasmaster.mysql.database.azure.com',3306,1,1,'Write Group');
insert into mysql_servers(hostgroup_id,hostname,port,weight,use_ssl, comment) 
  values(20,'mysqlpaasreplica1.mysql.database.azure.com',3306,1,1,'Read Group');
insert into mysql_servers(hostgroup_id,hostname,port,weight,use_ssl, comment) 
  values(20,'mysqlpaasreplica2.mysql.database.azure.com',3306,1,1,'Read Group');
</pre>
We then define the 'sbtest' proxysql user that  will transparently be transformed at connection time. ProxySQL will inject the database name in the username as is required by Azure Database for MySQL to connect through the gateway. 'sbtest' username  will be transformed to sbtest@mysqlpaasmaster, sbtest@mysqlpaasreplica1, sbtest@mysqlpaasreplica2 depending on what backend proxysql connect to. The 'sbtest' user has for default host group 10 which is the master server. That means that all queries for which no routing rules applies will end there.

<pre lang="bash" cssfile="another_style" >
insert into mysql_users(username,password,default_hostgroup,transaction_persistent) 
  values('sbtest','Passw0rd',10,1);
</pre>
No we need to define  the query routing rules that will determine to which host groups and consequently backends the queries will be routed. For Read/Write splitting that is quite simple : SELECT FOR UPDATE to 'Write group, SELECT to 'Read group' and all the rest to the default group of the user. So this means everything to 'Write group' except pure SELECT. 
<pre lang="bash" cssfile="another_style" >
insert into mysql_query_rules(rule_id,active,match_digest,destination_hostgroup,apply) 
  values(1,1,'^SELECT.*FOR UPDATE$',10,1); 
insert into mysql_query_rules(rule_id,active,match_digest,destination_hostgroup,apply) 
  values(2,1,'^SELECT',20,1); 
</pre>
Our setup for proxysql is in memory and need to be pushed to runtime an disk. 
<pre lang="bash" cssfile="another_style" >
load mysql users to runtime; 
load mysql servers to runtime; 
load mysql query rules to runtime; 
load mysql variables to runtime; 
load admin variables to runtime; 

save mysql users to disk; 
save mysql servers to disk; 
save mysql query rules to disk; 
save mysql variables to disk; 
save admin variables to disk; 
</pre>










================================




In the <a href="https://serge.frezefond.com/2020/06/using-proxysql-with-azure-database-for-mysql-mariadb/" rel="noopener" target="_blank">previous post</a> I mentioned a hack to make <a href="https://proxysql.com/documentation/" rel="noopener" target="_blank">ProxySQL</a> compatible with <a href="https://docs.microsoft.com/en-us/azure/mysql/" rel="noopener" target="_blank">Azure Database for MySQL</a>. If you want to try it you can download it from <a href="https://github.com/sfrezefo/proxysql/tree/azurehack" rel="noopener" target="_blank">github</a> and build a package for your target linux distribution :
<pre lang="bash" cssfile="another_style" >
$ git clone https://github.com/sfrezefo/proxysql.git
$ cd  proxysql
$ git checkout azurehack
</pre>

To make a usable package you just need to have docker available. The build process through the Makefile trigger a docker container which already has all the required dependencies for building installed. For example to make a package for ubuntu 18, to install it and to run it :
<pre lang="bash" cssfile="another_style" >
$ make ubuntu18
$ cd binaries
$ dpkg -i proxysql_2.0.13-ubuntu18_amd64.deb
$ sudo service proxysql start
</pre>

We now have a running proxysql. Let us use it. We first create a master and 2 replicas Azure Database for MySQL. We connect to the master and create 2 users : 'sbtest' for injecting traffic and 'monitoruser' required by proxysql to monitor the backend servers :
 
<pre lang="sql" cssfile="another_style" >
mysql -h mysqlpaasmaster.mysql.database.azure.com -u sbtest@mysqlpaasmaster -p \
  --ssl-mode=REQUIRED

CREATE SCHEMA sbtest;
CREATE USER sbtest@'%' IDENTIFIED BY 'Passw0rd';
GRANT ALL PRIVILEGES ON sbtest.* to sbtest@'%';

CREATE USER 'monitoruser'@'%' IDENTIFIED BY 'Passw0rd'; 
GRANT SELECT ON *.* TO 'monitoruser'@'%' WITH GRANT OPTION; 
FLUSH PRIVILEGES; 
</pre>
Now we configure proxysql through the proxysql admin. At initial startup proxysql reads its configuration from /etc/proxysql.cnf. This is where the admin user credentials are defined :
<pre lang="bash" cssfile="another_style" >
        admin_credentials="proxysqladmin:Passw0rd"
        mysql_ifaces="0.0.0.0:6032"
</pre>
All the rest of the configuration can be done in a scripted way that will be persisted to disk in a SQLite database.
<pre lang="bash" cssfile="another_style" >
mysql -h 127.0.0.1  -u proxysqladmin -pPassw0rd -P6032 --ssl

set mysql-monitor_username='monitoruser'; 
set mysql-monitor_password='Passw0rd'; 
</pre>

Before defining the servers we activate the azure feature so that they can be reached by proxysql monitoring through a common monitoring user. 
<pre lang="bash" cssfile="another_style" >
set mysql-azure_gen1_username='true';

insert into mysql_servers(hostgroup_id,hostname,port,weight,use_ssl, comment) 
  values(10,'mysqlpaasmaster.mysql.database.azure.com',3306,1,1,'Write Group');
insert into mysql_servers(hostgroup_id,hostname,port,weight,use_ssl, comment) 
  values(20,'mysqlpaasreplica1.mysql.database.azure.com',3306,1,1,'Read Group');
insert into mysql_servers(hostgroup_id,hostname,port,weight,use_ssl, comment) 
  values(20,'mysqlpaasreplica2.mysql.database.azure.com',3306,1,1,'Read Group');
</pre>
We then define the 'sbtest' proxysql user that  will transparently be transformed at connection time. ProxySQL will inject the database name in the username as is required by Azure Database for MySQL to connect through the gateway. 'sbtest' username  will be transformed to sbtest@mysqlpaasmaster, sbtest@mysqlpaasreplica1, sbtest@mysqlpaasreplica2 depending on what backend proxysql connect to. The 'sbtest' user has for default host group 10 which is the master server. That means that all queries for which no routing rules applies will end there.

<pre lang="bash" cssfile="another_style" >
insert into mysql_users(username,password,default_hostgroup,transaction_persistent) 
  values('sbtest','Passw0rd',10,1);
</pre>
No we need to define  the query routing rules that will determine to which host groups and consequently backends the queries will be routed. For Read/Write splitting that is quite simple : SELECT FOR UPDATE to 'Write group, SELECT to 'Read group' and all the rest to the default group of the user. So this means everything to 'Write group' except pure SELECT. 
<pre lang="bash" cssfile="another_style" >
insert into mysql_query_rules(rule_id,active,match_digest,destination_hostgroup,apply) 
  values(1,1,'^SELECT.*FOR UPDATE$',10,1); 
insert into mysql_query_rules(rule_id,active,match_digest,destination_hostgroup,apply) 
  values(2,1,'^SELECT',20,1); 
</pre>
Our setup for proxysql is in memory and need to be pushed to runtime an disk. 
<pre lang="bash" cssfile="another_style" >
load mysql users to runtime; 
load mysql servers to runtime; 
load mysql query rules to runtime; 
load mysql variables to runtime; 
load admin variables to runtime; 

save mysql users to disk; 
save mysql servers to disk; 
save mysql query rules to disk; 
save mysql variables to disk; 
save admin variables to disk; 
</pre>

To test our configuration we need to inject traffic. We will use sysbench for that :
<pre lang="bash" cssfile="another_style" >
  sysbench --threads=4 '/usr/share/sysbench/oltp_read_write.lua' \
            --db-driver=mysql --time=20 \
            --mysql-host='127.0.0.1' --mysql-port=3306 \
            --mysql-user=sbtest --mysql-password=Passw0rd \
            --tables=5 --tables=10000 \
            prepare

  sysbench --threads=4 '/usr/share/sysbench/oltp_read_write.lua' \
            --db-driver=mysql --time=20 \
            --mysql-host='127.0.0.1' --mysql-port=3306 \
            --mysql-user=sbtest --mysql-password=Passw0rd \
            --tables=5 --tables=10000 \
            run
</pre>

We see that master and replicas have received their share of sysbench queries.
<pre lang="sql" cssfile="another_style" >
MySQL > select hostgroup, srv_host,Queries from stats_mysql_connection_pool;
+-----------+--------------------------------------------+---------+
| hostgroup | srv_host                                   | Queries |
+-----------+--------------------------------------------+---------+
| 10        | mysqlpaasmaster.mysql.database.azure.com   | 472     |
| 20        | mysqlpaasreplica1.mysql.database.azure.com | 415     |
| 20        | mysqlpaasmaster.mysql.database.azure.com   | 402     |
| 20        | mysqlpaasreplica2.mysql.database.azure.com | 422     |
+-----------+--------------------------------------------+---------+
4 rows in set (0.00 sec).
</pre>
We also get the digest of all the queries run and on which hostgroup they ran. We can see here that all INSERT, UPDATE,DELE were sent to the Write hostgroup and the SELECT to the Read hostgroup.
<pre lang="sql" cssfile="another_style" >
select hostgroup, username, digest_text from  stats_mysql_query_digest;
+-----------+----------+-------------------------------------------------------------+
| hostgroup | username | digest_text                                                 |
+-----------+----------+-------------------------------------------------------------+
| 10        | sbtest   | INSERT INTO sbtest5 (id, k, c, pad) VALUES (?, ?, ?, ?)     |
| 10        | sbtest   | DELETE FROM sbtest2 WHERE id=?                              |
| 10        | sbtest   | UPDATE sbtest2 SET c=? WHERE id=?                           |
| 20        | sbtest   | SELECT c FROM sbtest5 WHERE id BETWEEN ? AND ? ORDER BY c   |
| 20        | sbtest   | SELECT SUM(k) FROM sbtest4 WHERE id BETWEEN ? AND ?         |
...
</pre>

I hope this helped.





Azure Database for MySQL is a PaaS offer. It has a specific architecture that relies on a gateway. This has a huge advantage in the way it handle  High availability. If a server fails it will automatically restart. The storage for the database is highly resilient and will be reconnected to the new server. You get HA out of the box without having to care about replica and failover handling.

if we look at a connection to a Azure Database for MySQL it is different from a usual MySQL connection.

<pre lang="bashl" cssfile="another_style" >
mysql -h mysqlpaasmaster.mysql.database.azure.com \
  -u sbtest@mysqlpaasmaster -p \
  --ssl-mode=REQUIRED
 </pre>


  we notice :
  hostname : mysqlpaasmaster.mysql.database.azure.com 
  username : sbtest@mysqlpaasmaster
  
  Why do we have the instance name in the username ?
  If we look at what the host name is, using the unix host command (dig would also do the trick).

<pre lang="bash" cssfile="another_style" >
$ host mysqlpaasmaster.mysql.database.azure.com
mysqlpaasmaster.mysql.database.azure.com is an alias for cr5.northeurope1-a.control.database.windows.net.
cr5.northeurope1-a.control.database.windows.net has address 52.138.224.6
 </pre>

The host name is just an alias to a gateway server (it is not an A record in the DNS). So the host you connect to is specific to the database's region but carry no information about the mysql instance you connect to. This explains why when you connect you need to embed the database name into the user name. This is the only way for the gateway to know which instance you want to connect to.

Does this fit with <a href="https://proxysql.com/documentation/" rel="noopener" target="_blank">proxySQL</a> ? unfortunately <strong>No</strong>.

ProxySQL is a fantastic technology widely used on MySQL / MariaDB architectures on premise or in the cloud. It has a nice design with the concept of host groups and query rules used to route queries to the desired backend server (based on port or regex).

To achieve this routing proxySQL uses a set of users that will potentially connect to multiple  backends depending on the status of these backends and the routing query rules. This is also the same for the monitoring user that is common to all the backends.

With Azure Database for MySQL we have a different user for each instances. This  does not fit the ProxySQL connection to backends pattern.

How to fix this ? I decided to try a little hack ;-) !

The idea is to keep the principle of having a common user for all backends. ProxySQL will transparently at connection time inject the database name in the username. 

For example for a username defined as <strong>'sbtest'</strong> it will generate different usernames to be used to connect
<pre lang="bash" cssfile="another_style" >
sbtest@mysqlpaasmaster if connecting to mysqlpaasmaster.mysql.database.azure.com
sbtest@mysqlpaasreplica1 if connecting to mysqlpaasreplica1.mysql.database.azure.com
sbtest@mysqlpaasreplica2 if connecting to mysqlpaasreplica2.mysql.database.azure.com
</pre>

it will do the same when connecting to the monitoring user <strong>'monitoruser'</strong>
<pre lang="bash" cssfile="another_style" >
monitoruser@mysqlpaasmaster if connecting to mysqlpaasmaster.mysql.database.azure.com
monitoruser@mysqlpaasreplica1 if connecting to mysqlpaasreplica1.mysql.database.azure.com
monitoruser@mysqlpaasreplica2 if connecting to mysqlpaasreplica2.mysql.database.azure.com
</pre>

So now to test this hack with Azure Database for MySQL I have  setup a Master with 2 replicas (that can be geo replica in another region if you wish). I have  created a single user 'sbtest' in proxySQL. On this setup I run a simple sysbench to inject traffic. I use the oltp_read_write.lua script to generate insert, update, delete and select to validate that the read write splitting is working correctly. And it works like a charm :-)

Here are the host groups, 10 for writes and 20 for reads. Hostgroup 20 contains the 2 replicas plus the master that can also be used for reads(if you want it to focus on write you can put a low weight). Hostgroup 10 contains only the master :
<pre lang="sql" cssfile="another_style" >
MySQL > select hostgroup_id,hostname,status,comment,use_ssl from mysql_servers;
+--------------+--------------------------------------------+--------+-------------+---+
| hostgroup_id | hostname                                   | status | comment     |use_ssl
+--------------+-----------------,---------------------------+--------+-------------+----
| 10           | mysqlpaasmaster.mysql.database.azure.com   | ONLINE | Write Group | 1 |
| 20           | mysqlpaasreplica1.mysql.database.azure.com | ONLINE | Read Group  | 1 |
| 20           | mysqlpaasreplica2.mysql.database.azure.com | ONLINE | Read Group  | 1 |
| 20           | mysqlpaasmaster.mysql.database.azure.com   | ONLINE | Write Group | 1 |
+--------------+--------------------------------------------+--------+-------------+---+
4 rows in set (0.00 sec)
</pre>
Here is the single user used for all the backends.
<pre lang="sql" cssfile="another_style" >
MySQL > select username,password,active,use_ssl,default_hostgroup from mysql_users;
+----------+----------+--------+---------+-------------------+
| username | password | active | use_ssl | default_hostgroup |
+----------+----------+--------+---------+-------------------+
| sbtest   | password | 1      | 0       | 10                | 
+----------+----------+--------+---------+-------------------+
1 row in set (0.00 sec)
</pre>
And here are the query rules to route the queries to the right backend.
<pre lang="sql" cssfile="another_style" >
MySQL >  select rule_id,match_digest,destination_hostgroup from mysql_query_rules;
+---------+-----------------------+-----------------------+
| rule_id | match_digest          | destination_hostgroup |
+---------+-----------------------+-----------------------+
| 1       | ^SELECT .* FOR UPDATE | 10                    |
| 2       | ^SELECT .*            | 20                    |
+---------+-----------------------+-----------------------+
2 rows in set (0.00 sec)
</pre>
Metrics data has also been collected inside the stats schema. We see that master and replicas have received their share of sysbench queries.
<pre lang="sql" cssfile="another_style" >
MySQL > select hostgroup, srv_host,Queries from stats_mysql_connection_pool;
+-----------+--------------------------------------------+---------+
| hostgroup | srv_host                                   | Queries |
+-----------+--------------------------------------------+---------+
| 10        | mysqlpaasmaster.mysql.database.azure.com   | 472     |
| 20        | mysqlpaasreplica1.mysql.database.azure.com | 415     |
| 20        | mysqlpaasmaster.mysql.database.azure.com   | 402     |
| 20        | mysqlpaasreplica2.mysql.database.azure.com | 422     |
+-----------+--------------------------------------------+---------+
4 rows in set (0.00 sec).
</pre>
Through the stats we also get the digest of all the queries run and on wich hostgroup they ran. We can see here that all INSERT, UPDATE,DELE were sent to the Write hostgroup and the SELECT to the Read hostgroup.
<pre lang="sql" cssfile="another_style" >
select hostgroup, username, digest_text from  stats_mysql_query_digest;
+-----------+----------+-------------------------------------------------------------+
| hostgroup | username | digest_text                                                 |
+-----------+----------+-------------------------------------------------------------+
| 10        | sbtest   | INSERT INTO sbtest5 (id, k, c, pad) VALUES (?, ?, ?, ?)     |
| 10        | sbtest   | DELETE FROM sbtest2 WHERE id=?                              |
| 10        | sbtest   | UPDATE sbtest2 SET c=? WHERE id=?                           |
| 20        | sbtest   | SELECT c FROM sbtest5 WHERE id BETWEEN ? AND ? ORDER BY c   |
| 20        | sbtest   | SELECT SUM(k) FROM sbtest4 WHERE id BETWEEN ? AND ?         |
...
</pre>

In the monitor schema we will find data that has been collected by the 'monitoruser' that has been correctly map to the correct Azure username. In the monitor schema we can find log data for connect, ping, read_only ... Here for example the ping data to check the availability of the backends :
<pre lang="sql" cssfile="another_style" >
MySQL > select hostname from mysql_server_ping_log;
+--------------------------------------------+------+------------------+------------+
| hostname                                   | port | time_start_us    | ping_success_time_us |
+--------------------------------------------+------+------------------+------------+
| mysqlpaasreplica1.mysql.database.azure.com | 3306 | 1591785759257052 | 20088      |
| mysqlpaasreplica2.mysql.database.azure.com | 3306 | 1591785759269801 | 19948      |
| mysqlpaasmaster.mysql.database.azure.com   | 3306 | 1591785759282430 | 19831      | 
</pre>
To use this Azure specific username rewriting hack I have defined a global variable that activate or not this Azure specific code. By default it is false which means nothing specific happens.
<pre lang="sql" cssfile="another_style" >
select variable_value from global_variables where variable_name='mysql-azure_gen1_username';
+----------------+
| variable_value |
+----------------+
| false          |
+----------------+
1 row in set (0.00 sec)
</pre>
To activate the Azure specific behavior when using Azure Database for MySQL / MariaDB:
<pre lang="bash" cssfile="another_style" >
set mysql-azure_gen1_username='true';
load mysql variables to runtime; ONLINE
</pre>

if you need the code you can get it from my repo <a href="https://github.com/sfrezefo/proxysql/tree/azurehack" rel="noopener" target="_blank">https://github.com/sfrezefo/proxysql/tree/azurehack</a>
As I am a occasional developper feel free to improve/fix ;-)
I will submit a pull request to the Rene_Canao/proxySQL repo.  
I hope this will help the use of proxySQL on Azure with Azure Database for MySQL / MariaDB.
