\set VERBOSITY terse
\pset format unaligned

-- predictability
SET synchronous_commit = on;

DROP TABLE IF EXISTS table_with_unique;

CREATE TABLE table_with_unique (
a	smallserial,
b	smallint,
c	int,
d	bigint,
e	numeric(5,3) not null,
f	real not null,
g	double precision not null,
h	char(10),
i	varchar(30),
j	text,
k	bit varying(20),
l	timestamp,
m	date,
n	boolean not null,
o	json,
p	tsvector,
UNIQUE(g, n)
);

-- INSERT
INSERT INTO table_with_unique (b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) VALUES(1, 2, 3, 3.54, 876.563452345, 1.23, 'teste', 'testando', 'um texto longo', B'001110010101010', '2013-11-02 17:30:52', '2013-02-04', false, '{ "a": 123 }', 'Old Old Parr'::tsvector);
INSERT INTO table_with_unique (b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) VALUES(4, 5, 6, 3.54, 876.563452345, 4.56, 'teste', 'testando', 'um texto longo', B'001110010101010', '2013-11-02 17:30:52', '2013-02-04', true, '{ "a": 123 }', 'Old Old Parr'::tsvector);

SELECT slot_create();

-- DELETE: REPLICA IDENTITY INDEX
ALTER TABLE table_with_unique REPLICA IDENTITY USING INDEX table_with_unique_g_n_key;
DELETE FROM table_with_unique WHERE b = 1;
DELETE FROM table_with_unique WHERE n = true;
ALTER TABLE table_with_unique REPLICA IDENTITY DEFAULT;

SELECT data FROM slot_get();
SELECT slot_drop();
