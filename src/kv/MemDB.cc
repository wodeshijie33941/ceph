// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * In-memory crash non-safe keyvalue db
 * Author: Ramesh Chander, Ramesh.Chander@sandisk.com
 */

#include <set>
#include <map>
#include <string>
#include <memory>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "common/perf_counters.h"
#include "common/debug.h"
#include "include/str_list.h"
#include "include/str_map.h"
#include "KeyValueDB.h"
#include "MemDB.h"

#include "include/assert.h"
#include "common/debug.h"
#include "common/errno.h"
#include "include/compat.h"

#define dout_subsys ceph_subsys_memdb
#undef dout_prefix
#define dout_prefix *_dout << "memdb: "
#define dtrace dout(30)
#define dwarn dout(0)
#define dinfo dout(0)

static void split_key(const string& raw_key, string *prefix, string *key)
{
  size_t pos = raw_key.find(KEY_DELIM, 0);
  *prefix = raw_key.substr(0, pos);
  *key = raw_key.substr(pos + 1, raw_key.length());
}

static string make_key(const string &prefix, const string &value)
{
  string out = prefix;
  out.push_back(KEY_DELIM);
  out.append(value);
  return out;
}

void MemDB::_encode(btree::btree_map<string,
                      bufferptr>:: iterator iter, bufferlist &bl)
{
  ::encode(iter->first, bl);
  ::encode(iter->second, bl);
}

std::string MemDB::_get_data_fn()
{
  string fn = m_db_path + "/" + "MemDB.db";
  return fn;
}

void MemDB::_save()
{
  std::lock_guard<std::mutex> l(m_lock);
  dout(10) << __func__ << " Saving MemDB to file: "<< _get_data_fn().c_str() << dendl;
  int mode = 0644;
  int fd = TEMP_FAILURE_RETRY(::open(_get_data_fn().c_str(),
                                     O_WRONLY|O_CREAT|O_TRUNC, mode));
  if (fd < 0) {
    int err = errno;
    cerr << "write_file(" << _get_data_fn().c_str() << "): failed to open file: "
         << cpp_strerror(err) << std::endl;
    return;
  }
  btree::btree_map<string, bufferptr>::iterator iter = m_btree.begin();
  while (iter != m_btree.end()) {
    bufferlist bl;
    dout(10) << __func__ << " Key:"<< iter->first << dendl;
    _encode(iter, bl);
    bl.write_fd(fd);
    iter++;
  }

  VOID_TEMP_FAILURE_RETRY(::close(fd));
}

void MemDB::_load()
{
  std::lock_guard<std::mutex> l(m_lock);
  dout(10) << __func__ << " Reading MemDB from file: "<< _get_data_fn().c_str() << dendl;
  /*
   * Open file and read it in single shot.
   */
  int fd = TEMP_FAILURE_RETRY(::open(_get_data_fn().c_str(), O_RDONLY));
  if (fd < 0) {
    int err = errno;
    std::ostringstream oss;
    oss << "can't open " << _get_data_fn().c_str() << ": "
        << cpp_strerror(err) << std::endl;
    return;
  }

  struct stat st;
  memset(&st, 0, sizeof(st));
  if (::fstat(fd, &st) < 0) {
    int err = errno;
    std::ostringstream oss;
    oss << "can't stat file " << _get_data_fn().c_str() << ": "
        << cpp_strerror(err) << std::endl;
    VOID_TEMP_FAILURE_RETRY(::close(fd));
    return;
  }

  ssize_t file_size = st.st_size;
  ssize_t bytes_done = 0;
  while (bytes_done < file_size) {
    string key;
    bufferptr datap;

    bytes_done += ::decode_file(fd, key);
    bytes_done += ::decode_file(fd, datap);

    dout(10) << __func__ << " Key:"<< key << dendl;
    m_btree[key] = datap;
  }
  VOID_TEMP_FAILURE_RETRY(::close(fd));
}

int MemDB::_init(bool create)
{
  dout(1) << __func__ << dendl;
  if (create) {
    int r = ::mkdir(m_db_path.c_str(), 0700);
    if (r < 0) {
      r = -errno;
      if (r != -EEXIST) {
        derr << __func__ << " mkdir failed: " << cpp_strerror(r) << dendl;
        return r;
      }
    }
 } else {
    _load();
 }

  return 0;
}

int MemDB::set_merge_operator(
  const string& prefix,
  std::shared_ptr<KeyValueDB::MergeOperator> mop)
{
  merge_ops.push_back(std::make_pair(prefix, mop));
  return 0;
}

int MemDB::do_open(ostream &out, bool create)
{
  m_total_bytes = 0;
  m_allocated_bytes = 1;

  return _init(create);
}

MemDB::~MemDB()
{
  close();
  dout(10) << __func__ << " Destroying MemDB instance: "<< dendl;
}

void MemDB::close()
{
  /*
   * Save whatever in memory btree.
   */
  _save();
}

int MemDB::submit_transaction(KeyValueDB::Transaction t)
{
  MDBTransactionImpl* mt =  static_cast<MDBTransactionImpl*>(t.get());

  dtrace << __func__ << " " << mt->get_ops().size() << dendl;
  for(auto& op : mt->get_ops()) {
    if(op.first == MDBTransactionImpl::WRITE) {
      ms_op_t set_op = op.second;
      _setkey(set_op);
    } else if (op.first == MDBTransactionImpl::MERGE) {
      ms_op_t merge_op = op.second;
      _merge(merge_op);
    } else {
      ms_op_t rm_op = op.second;
      assert(op.first == MDBTransactionImpl::DELETE);
      _rmkey(rm_op);
    }
  }

  return 0;
}

int MemDB::submit_transaction_sync(KeyValueDB::Transaction tsync)
{
  dtrace << __func__ << " " << dendl;
  submit_transaction(tsync);
  return 0;
}

int MemDB::transaction_rollback(KeyValueDB::Transaction t)
{
  MDBTransactionImpl* mt =  static_cast<MDBTransactionImpl*>(t.get());
  mt->clear();
  return 0;
}

void MemDB::MDBTransactionImpl::set(
  const string &prefix, const string &k, const bufferlist &to_set_bl)
{
  dtrace << __func__ << " " << prefix << " " << k << dendl;
  ops.push_back(make_pair(WRITE, std::make_pair(std::make_pair(prefix, k),
                  to_set_bl)));
}

void MemDB::MDBTransactionImpl::rmkey(const string &prefix,
    const string &k)
{
  dtrace << __func__ << " " << prefix << " " << k << dendl;
  ops.push_back(make_pair(DELETE,
                          std::make_pair(std::make_pair(prefix, k),
                          NULL)));
}

void MemDB::MDBTransactionImpl::rmkeys_by_prefix(const string &prefix)
{
  KeyValueDB::Iterator it = m_db->get_iterator(prefix);
  for (it->seek_to_first(); it->valid(); it->next()) {
    rmkey(prefix, it->key());
  }
}

void MemDB::MDBTransactionImpl::merge(
  const std::string &prefix, const std::string &key, const bufferlist  &value)
{

  dtrace << __func__ << " " << prefix << " " << key << dendl;
  ops.push_back(make_pair(MERGE, make_pair(std::make_pair(prefix, key), value)));
  return;
}

int MemDB::_setkey(ms_op_t &op)
{
  std::lock_guard<std::mutex> l(m_lock);
  std::string key = make_key(op.first.first, op.first.second);
  bufferlist bl = op.second;

  m_total_bytes += bl.length();

  bufferlist bl_old;
  if (_get(op.first.first, op.first.second, &bl_old)) {
    /*
     * delete and free existing key.
     */
    m_total_bytes -= bl_old.length();
    m_btree.erase(key);
  }

  m_btree[key] = bufferptr((char *) bl.c_str(), bl.length());

  return 0;
}

int MemDB::_rmkey(ms_op_t &op)
{
  std::lock_guard<std::mutex> l(m_lock);
  std::string key = make_key(op.first.first, op.first.second);

  bufferlist bl_old;
  if (_get(op.first.first, op.first.second, &bl_old)) {
    m_total_bytes -= bl_old.length();
  }
  /*
   * Erase will call the destructor for bufferptr.
   */
  return m_btree.erase(key);
}

std::shared_ptr<KeyValueDB::MergeOperator> MemDB::_find_merge_op(std::string prefix)
{
  for (const auto& i : merge_ops) {
    if (i.first == prefix) {
      return i.second;
    }
  }

  dtrace << __func__ << " No merge op for " << prefix << dendl;
  return NULL;
}


int MemDB::_merge(ms_op_t &op)
{
  std::lock_guard<std::mutex> l(m_lock);
  std::string prefix = op.first.first;
  std::string key = make_key(op.first.first, op.first.second);
  bufferlist bl = op.second;
  int64_t bytes_adjusted = bl.length();

  /*
   *  find the operator for this prefix
   */
  std::shared_ptr<MergeOperator> mop = _find_merge_op(prefix);
  assert(mop);

  /*
   * call the merge operator with value and non value
   */
  bufferlist bl_old;
  if (_get(op.first.first, op.first.second, &bl_old) == false) {
    std::string new_val;
    /*
     * Merge non existent.
     */
    mop->merge_nonexistent(bl.c_str(), bl.length(), &new_val);
    m_btree[key] = bufferptr(new_val.c_str(), new_val.length());
  } else {
    /*
     * Merge existing.
     */
    std::string new_val;
    mop->merge(bl_old.c_str(), bl_old.length(), bl.c_str(), bl.length(), &new_val);
    m_btree[key] = bufferptr(new_val.c_str(), new_val.length());
    bytes_adjusted -= bl_old.length();
    bl_old.clear();
  }

  m_total_bytes += bytes_adjusted;
  return 0;
}

/*
 * Caller take btree lock.
 */
bool MemDB::_get(const string &prefix, const string &k, bufferlist *out)
{
  string key = make_key(prefix, k);

  btree::btree_map<string, bufferptr>::iterator iter = m_btree.find(key);
  if (iter == m_btree.end()) {
    return false;
  }

  out->push_back((m_btree[key].clone()));
  return true;
}

bool MemDB::_get_locked(const string &prefix, const string &k, bufferlist *out)
{
  std::lock_guard<std::mutex> l(m_lock);
  return _get(prefix, k, out);
}


int MemDB::get(const string &prefix, const std::string& key,
                 bufferlist *out)
{
  if (_get_locked(prefix, key, out)) {
    return 0;
  }
  return -ENOENT;
}

int MemDB::get(const string &prefix, const std::set<string> &keys,
    std::map<string, bufferlist> *out)
{
  for (const auto& i : keys) {
    bufferlist bl;
    if (_get_locked(prefix, i, &bl))
      out->insert(make_pair(i, bl));
  }

  return 0;
}

void MemDB::MDBWholeSpaceIteratorImpl::fill_current()
{
  bufferlist bl;
  bl.append(m_iter->second.clone());
  m_key_value = std::make_pair(m_iter->first, bl);
}

bool MemDB::MDBWholeSpaceIteratorImpl::valid()
{
  if (m_key_value.first.empty()) {
    return false;
  }
  return true;
}

void
MemDB::MDBWholeSpaceIteratorImpl::free_last()
{
  m_key_value.first.clear();
  assert(m_key_value.first.empty());
  m_key_value.second.clear();
}

string MemDB::MDBWholeSpaceIteratorImpl::key()
{
  dtrace << __func__ << " " << m_key_value.first << dendl;
  string prefix, key;
  split_key(m_key_value.first, &prefix, &key);
  return key;
}

pair<string,string> MemDB::MDBWholeSpaceIteratorImpl::raw_key()
{
  string prefix, key;
  split_key(m_key_value.first, &prefix, &key);
  return make_pair(prefix, key);
}

bool MemDB::MDBWholeSpaceIteratorImpl::raw_key_is_prefixed(
    const string &prefix)
{
  string p, k;
  split_key(m_key_value.first, &p, &k);
  return (p == prefix);
}

bufferlist MemDB::MDBWholeSpaceIteratorImpl::value()
{
  dtrace << __func__ << " " << m_key_value << dendl;
  return m_key_value.second;
}

int MemDB::MDBWholeSpaceIteratorImpl::next()
{
  std::lock_guard<std::mutex> l(*m_btree_lock_p);
  free_last();
  m_iter++;
  if (m_iter != m_btree_p->end()) {
    fill_current();
    return 0;
  } else {
    return -1;
  }
}

int MemDB::MDBWholeSpaceIteratorImpl:: prev()
{
  std::lock_guard<std::mutex> l(*m_btree_lock_p);
  free_last();
  m_iter--;
  if (m_iter != m_btree_p->end()) {
    fill_current();
    return 0;
  } else {
    return -1;
  }
}

/*
 * First key >= to given key, if key is null then first key in btree.
 */
int MemDB::MDBWholeSpaceIteratorImpl::seek_to_first(const std::string &k)
{
  std::lock_guard<std::mutex> l(*m_btree_lock_p);
  free_last();
  if (k.empty()) {
    m_iter = m_btree_p->begin();
  } else {
    m_iter = m_btree_p->lower_bound(k);
  }

  if (m_iter == m_btree_p->end()) {
    return -1;
  }
  return 0;
}

int MemDB::MDBWholeSpaceIteratorImpl::seek_to_last(const std::string &k)
{
  std::lock_guard<std::mutex> l(*m_btree_lock_p);

  free_last();
  if (k.empty()) {
    m_iter = m_btree_p->end();
    m_iter--;
  } else {
    m_iter = m_btree_p->lower_bound(k);
  }

  if (m_iter == m_btree_p->end()) {
    return -1;
  }
  return 0;
}

MemDB::MDBWholeSpaceIteratorImpl::~MDBWholeSpaceIteratorImpl()
{
  free_last();
}

KeyValueDB::WholeSpaceIterator MemDB::_get_snapshot_iterator()
{
  assert(0);
}

int MemDB::MDBWholeSpaceIteratorImpl::upper_bound(const std::string &prefix,
    const std::string &after) {

  std::lock_guard<std::mutex> l(*m_btree_lock_p);

  dtrace << "upper_bound " << prefix.c_str() << after.c_str() << dendl;
  string k = make_key(prefix, after);
  m_iter = m_btree_p->upper_bound(k);
  if (m_iter != m_btree_p->end()) {
    fill_current();
    return 0;
  }
  return -1;
}

int MemDB::MDBWholeSpaceIteratorImpl::lower_bound(const std::string &prefix,
    const std::string &to) {
  std::lock_guard<std::mutex> l(*m_btree_lock_p);
  dtrace << "lower_bound " << prefix.c_str() << to.c_str() << dendl;
  string k = make_key(prefix, to);
  m_iter = m_btree_p->lower_bound(k);
  if (m_iter != m_btree_p->end()) {
    fill_current();
    return 0;
  }
  return -1;
}
