-- Ready2Gear · สร้างตารางใน PostgreSQL
-- รันในฐานข้อมูลชื่อ ready2gear (สร้างก่อนด้วย: CREATE DATABASE ready2gear;)

CREATE TABLE IF NOT EXISTS workers (
  id            serial PRIMARY KEY,
  employee_code text UNIQUE NOT NULL,          -- ส่งให้บอร์ดเป็น employee_id
  name          text NOT NULL,                 -- ชื่อที่ขึ้นบนจอ (ภาษาอังกฤษ)
  face_label    text UNIQUE NOT NULL,          -- ชื่อคลาสใบหน้าในโมเดล CiRA (ต้องตรงตัวอักษร)
  active        boolean NOT NULL DEFAULT true
);

CREATE TABLE IF NOT EXISTS tools (
  id         serial PRIMARY KEY,
  name       text NOT NULL,                    -- ชื่อที่หัวหน้าเห็น
  cira_label text UNIQUE NOT NULL,             -- ชื่อคลาสของในโมเดล CiRA (ต้องตรงตัวอักษร)
  active     boolean NOT NULL DEFAULT true
);

CREATE TABLE IF NOT EXISTS jobs (
  id         serial PRIMARY KEY,
  job_date   date NOT NULL DEFAULT CURRENT_DATE,
  worker_id  int NOT NULL REFERENCES workers(id),
  title      text NOT NULL,
  status     text NOT NULL DEFAULT 'assigned'
             CHECK (status IN ('assigned','out','returned')),
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS job_tools (
  job_id  int NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
  tool_id int NOT NULL REFERENCES tools(id),
  qty     int NOT NULL CHECK (qty > 0),
  PRIMARY KEY (job_id, tool_id)
);

CREATE TABLE IF NOT EXISTS rounds (
  round_id  text PRIMARY KEY,                  -- มาจากบอร์ด
  worker_id int REFERENCES workers(id),
  job_id    int REFERENCES jobs(id),
  direction text NOT NULL CHECK (direction IN ('out','return')),
  complete  boolean NOT NULL,
  saved_at  timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS round_items (
  round_id text NOT NULL REFERENCES rounds(round_id) ON DELETE CASCADE,
  tool_id  int  NOT NULL REFERENCES tools(id),
  need     int  NOT NULL,
  got      int  NOT NULL,
  PRIMARY KEY (round_id, tool_id)
);

CREATE TABLE IF NOT EXISTS scans (
  request_id text PRIMARY KEY,                 -- มาจากบอร์ด (round_id-ลำดับ)
  round_id   text,                             -- ไม่ใส่ FK: สแกนเกิดก่อนบันทึกรอบ
  kind       text NOT NULL,                    -- face / item
  label      text,
  score      real,
  counts     jsonb,
  image      bytea,                            -- ภาพของ (ไม่เก็บภาพใบหน้า)
  created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS scans_round_idx ON scans(round_id);

-- ===== ข้อมูลตัวอย่าง (แก้ชื่อคลาสให้ตรงกับโมเดลของกลุ่ม) =====
INSERT INTO workers(employee_code, name, face_label) VALUES
  ('E001', 'Somchai', 'Somchai'),
  ('E002', 'Somsri',  'Somsri')
ON CONFLICT DO NOTHING;

INSERT INTO tools(name, cira_label) VALUES
  ('Focusta (ด้านหน้า)', 'Focustafront')
  -- , ('ค้อน', 'hammer')
  -- , ('ไขควง', 'screwdriver')
ON CONFLICT DO NOTHING;

-- งานวันนี้ของ Somchai: ต้องมี Focustafront 1 ชิ้น
WITH j AS (
  INSERT INTO jobs(job_date, worker_id, title)
  SELECT CURRENT_DATE, id, 'Test job' FROM workers WHERE employee_code = 'E001'
  RETURNING id
)
INSERT INTO job_tools(job_id, tool_id, qty)
SELECT j.id, t.id, 1 FROM j, tools t WHERE t.cira_label = 'Focustafront';
