
/*
NO source code changes:
jech DHT 0.25 https://github.com/jech/dht/commit/190517533c10db9da16b9ed2bfaceb80a4f872b6
introduced a bug https://github.com/jech/dht/commit/839fd73e26fc16f05e8a080f11d718d7ccdbdc16
which prevents replying to find_nodes when want was not set.
cfpp2p DHT 0.25 did NOT incorporate this bug.
For cfpp2p DHT 0.25, only appropriate positive changes for
cfpp2p transmission at DHT 0.25 were utilized.

Since
    *want_return = -1;
was kept intact, no bug and no change is nessesary as for DHT 0.26
See below.
*/


//cfpp2p DHT 0.25 -- NO change needed for DHT 0.26 December 17, 2018
//________________________________________________________________________
    if(want_return) {
        p = dht_memmem(buf, buflen, "4:wantl", 7);
        if(p) {
            int i = p - buf + 7;
            *want_return = 0;
            while(buf[i] > '0' && buf[i] <= '9' && buf[i + 1] == ':' &&
                  i + 2 + buf[i] - '0' < buflen) {
                CHECK(buf + i + 2, buf[i] - '0');
                if(buf[i] == '2' && memcmp(buf + i + 2, "n4", 2) == 0)
                    *want_return |= WANT4;
                else if(buf[i] == '2' && memcmp(buf + i + 2, "n6", 2) == 0)
                    *want_return |= WANT6;
                else
                    if(dht_debug) debugf("eek... unexpected want flag (%c)\n", buf[i]);
                i += 2 + buf[i] - '0';
            }
            if(i >= buflen || buf[i] != 'e')
                if(dht_debug) debugf("eek... unexpected end for want.\n");
        } else {
            *want_return = -1;
        }
    }

#undef CHECK
//________________________________________________________________________


//jech DHT 0.26 December 17, 2018
//________________________________________________________________________
    p = dht_memmem(buf, buflen, "4:wantl", 7);
    if(p) {
        int i = p - buf + 7;
        m->want = 0;
        while(buf[i] > '0' && buf[i] <= '9' && buf[i + 1] == ':' &&
              i + 2 + buf[i] - '0' < buflen) {
            CHECK(buf + i + 2, buf[i] - '0');
            if(buf[i] == '2' && memcmp(buf + i + 2, "n4", 2) == 0)
                m->want |= WANT4;
            else if(buf[i] == '2' && memcmp(buf + i + 2, "n6", 2) == 0)
                m->want |= WANT6;
            else
                debugf("eek... unexpected want flag (%c)\n", buf[i]);
            i += 2 + buf[i] - '0';
        }
        if(i >= buflen || buf[i] != 'e')
            debugf("eek... unexpected end for want.\n");
    }

#undef CHECK
//________________________________________________________________________


