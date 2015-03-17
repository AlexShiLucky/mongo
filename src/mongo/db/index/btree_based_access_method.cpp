/**
*    Copyright (C) 2013-2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/db/index/btree_access_method.h"

#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/btree_index_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"


namespace mongo {

    using std::endl;
    using std::set;
    using std::vector;

    MONGO_EXPORT_SERVER_PARAMETER(failIndexKeyTooLong, bool, true);

    //
    // Comparison for external sorter interface
    //

    // Defined in db/structure/btree/key.cpp
    // XXX TODO: rename to something more descriptive, etc. etc.
    int oldCompare(const BSONObj& l,const BSONObj& r, const Ordering &o);

    class BtreeExternalSortComparison {
    public:
        BtreeExternalSortComparison(const BSONObj& ordering, int version)
            : _ordering(Ordering::make(ordering)),
              _version(version) {
            invariant(version == 1 || version == 0);
        }

        typedef std::pair<BSONObj, RecordId> Data;

        int operator() (const Data& l, const Data& r) const {
            int x = (_version == 1
                        ? l.first.woCompare(r.first, _ordering, /*considerfieldname*/false)
                        : oldCompare(l.first, r.first, _ordering));
            if (x) { return x; }
            return l.second.compare(r.second);
        }
    private:
        const Ordering _ordering;
        const int _version;
    };

    BtreeBasedAccessMethod::BtreeBasedAccessMethod(IndexCatalogEntry* btreeState,
                                                   SortedDataInterface* btree)
        : _btreeState(btreeState),
          _descriptor(btreeState->descriptor()),
          _newInterface(btree) {
        verify(0 == _descriptor->version() || 1 == _descriptor->version());
    }

    bool BtreeBasedAccessMethod::ignoreKeyTooLong(OperationContext *txn) {
        // Ignore this error if we're on a secondary or if the user requested it
        return !txn->isPrimaryFor(_btreeState->ns()) || !failIndexKeyTooLong;
    }

    // Find the keys for obj, put them in the tree pointing to loc
    Status BtreeBasedAccessMethod::insert(OperationContext* txn,
                                          const BSONObj& obj,
                                          const RecordId& loc,
                                          const InsertDeleteOptions& options,
                                          int64_t* numInserted) {
        *numInserted = 0;

        BSONObjSet keys;
        // Delegate to the subclass.
        getKeys(obj, &keys);

        Status ret = Status::OK();
        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            Status status = _newInterface->insert(txn, *i, loc, options.dupsAllowed);

            // Everything's OK, carry on.
            if (status.isOK()) {
                ++*numInserted;
                continue;
            }

            // Error cases.

            if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(txn)) {
                continue;
            }

            if (status.code() == ErrorCodes::DuplicateKeyValue) {
                // A document might be indexed multiple times during a background index build
                // if it moves ahead of the collection scan cursor (e.g. via an update).
                if (!_btreeState->isReady(txn)) {
                    LOG(3) << "key " << *i << " already in index during background indexing (ok)";
                    continue;
                }
            }

            // Clean up after ourselves.
            for (BSONObjSet::const_iterator j = keys.begin(); j != i; ++j) {
                removeOneKey(txn, *j, loc, options.dupsAllowed);
                *numInserted = 0;
            }

            return status;
        }

        if (*numInserted > 1) {
            _btreeState->setMultikey( txn );
        }

        return ret;
    }

    void BtreeBasedAccessMethod::removeOneKey(OperationContext* txn,
                                              const BSONObj& key,
                                              const RecordId& loc,
                                              bool dupsAllowed) {
        try {
            _newInterface->unindex(txn, key, loc, dupsAllowed);
        } catch (AssertionException& e) {
            log() << "Assertion failure: _unindex failed "
                  << _descriptor->indexNamespace() << endl;
            log() << "Assertion failure: _unindex failed: " << e.what()
                  << "  key:" << key.toString()
                  << "  dl:" << loc;
            logContext();
        }
    }

    Status BtreeBasedAccessMethod::newCursor(OperationContext* txn, const CursorOptions& opts, IndexCursor** out) const {
        *out = new BtreeIndexCursor(_newInterface->newCursor(txn, opts.direction));
        return Status::OK();
    }

    // Remove the provided doc from the index.
    Status BtreeBasedAccessMethod::remove(OperationContext* txn,
                                          const BSONObj &obj,
                                          const RecordId& loc,
                                          const InsertDeleteOptions &options,
                                          int64_t* numDeleted) {

        BSONObjSet keys;
        getKeys(obj, &keys);
        *numDeleted = 0;

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            removeOneKey(txn, *i, loc, options.dupsAllowed);
            ++*numDeleted;
        }

        return Status::OK();
    }

    // Return keys in l that are not in r.
    // Lifted basically verbatim from elsewhere.
    static void setDifference(const BSONObjSet &l, const BSONObjSet &r, vector<BSONObj*> *diff) {
        // l and r must use the same ordering spec.
        verify(l.key_comp().order() == r.key_comp().order());
        BSONObjSet::const_iterator i = l.begin();
        BSONObjSet::const_iterator j = r.begin();
        while ( 1 ) {
            if ( i == l.end() )
                break;
            while ( j != r.end() && j->woCompare( *i ) < 0 )
                j++;
            if ( j == r.end() || i->woCompare(*j) != 0  ) {
                const BSONObj *jo = &*i;
                diff->push_back( (BSONObj *) jo );
            }
            i++;
        }
    }

    Status BtreeBasedAccessMethod::initializeAsEmpty(OperationContext* txn) {
        return _newInterface->initAsEmpty(txn);
    }

    Status BtreeBasedAccessMethod::touch(OperationContext* txn, const BSONObj& obj) {
        BSONObjSet keys;
        getKeys(obj, &keys);

        boost::scoped_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(txn, 1));
        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            cursor->locate(*i, RecordId());
        }

        return Status::OK();
    }


    Status BtreeBasedAccessMethod::touch( OperationContext* txn ) const {
        return _newInterface->touch(txn);
    }

    RecordId BtreeBasedAccessMethod::findSingle(OperationContext* txn, const BSONObj& key) const {
        boost::scoped_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(txn, 1));
        cursor->locate(key, RecordId::min());

        // A null bucket means the key wasn't found (nor was anything found after it).
        if (cursor->isEOF()) {
            return RecordId();
        }

        // We found something but it could be a key after 'key'.  Examine what we're pointing at.
        if (0 != key.woCompare(cursor->getKey(), BSONObj(), false)) {
            // If the keys don't match, return "not found."
            return RecordId();
        }

        // Return the RecordId found.
        return cursor->getRecordId();
    }

    Status BtreeBasedAccessMethod::validate(OperationContext* txn, bool full, int64_t* numKeys,
                                            BSONObjBuilder* output) {
        // XXX: long long vs int64_t
        long long keys = 0;
        _newInterface->fullValidate(txn, full, &keys, output);
        *numKeys = keys;
        return Status::OK();
    }

    bool BtreeBasedAccessMethod::appendCustomStats(OperationContext* txn,
                                                   BSONObjBuilder* output,
                                                   double scale) const {
        return _newInterface->appendCustomStats(txn, output, scale);
    }

    long long BtreeBasedAccessMethod::getSpaceUsedBytes( OperationContext* txn ) const {
        return _newInterface->getSpaceUsedBytes( txn );
    }

    Status BtreeBasedAccessMethod::validateUpdate(OperationContext* txn,
                                                  const BSONObj &from,
                                                  const BSONObj &to,
                                                  const RecordId &record,
                                                  const InsertDeleteOptions &options,
                                                  UpdateTicket* status) {

        BtreeBasedPrivateUpdateData *data = new BtreeBasedPrivateUpdateData();
        status->_indexSpecificUpdateData.reset(data);

        getKeys(from, &data->oldKeys);
        getKeys(to, &data->newKeys);
        data->loc = record;
        data->dupsAllowed = options.dupsAllowed;

        setDifference(data->oldKeys, data->newKeys, &data->removed);
        setDifference(data->newKeys, data->oldKeys, &data->added);

        status->_isValid = true;

        return Status::OK();
    }

    Status BtreeBasedAccessMethod::update(OperationContext* txn,
                                          const UpdateTicket& ticket,
                                          int64_t* numUpdated) {
        if (!ticket._isValid) {
            return Status(ErrorCodes::InternalError, "Invalid UpdateTicket in update");
        }

        BtreeBasedPrivateUpdateData* data =
            static_cast<BtreeBasedPrivateUpdateData*>(ticket._indexSpecificUpdateData.get());

        if (data->oldKeys.size() + data->added.size() - data->removed.size() > 1) {
            _btreeState->setMultikey( txn );
        }

        for (size_t i = 0; i < data->removed.size(); ++i) {
            _newInterface->unindex(txn,
                                   *data->removed[i],
                                   data->loc,
                                   data->dupsAllowed);
        }

        for (size_t i = 0; i < data->added.size(); ++i) {
            Status status = _newInterface->insert(txn,
                                                  *data->added[i],
                                                  data->loc,
                                                  data->dupsAllowed);
            if ( !status.isOK() ) {
                return status;
            }
        }

        *numUpdated = data->added.size();

        return Status::OK();
    }

    std::unique_ptr<IndexAccessMethod::BulkBuilder> BtreeBasedAccessMethod::initiateBulk() {

        return std::unique_ptr<BulkBuilder>(new BulkBuilder(this, _descriptor));
    }

    IndexAccessMethod::BulkBuilder::BulkBuilder(const BtreeBasedAccessMethod* index,
                                                const IndexDescriptor* descriptor)
            : _sorter(Sorter::make(SortOptions().TempDir(storageGlobalParams.dbpath + "/_tmp")
                                                .ExtSortAllowed()
                                                .MaxMemoryUsageBytes(100*1024*1024),
                                   BtreeExternalSortComparison(descriptor->keyPattern(),
                                                               descriptor->version())))
            , _real(index) {
    }

    Status IndexAccessMethod::BulkBuilder::insert(OperationContext* txn,
                                                  const BSONObj& obj,
                                                  const RecordId& loc,
                                                  const InsertDeleteOptions& options,
                                                  int64_t* numInserted) {
        BSONObjSet keys;
        _real->getKeys(obj, &keys);

        _isMultiKey = _isMultiKey || (keys.size() > 1);

        for (BSONObjSet::iterator it = keys.begin(); it != keys.end(); ++it) {
            _sorter->add(*it, loc);
            _keysInserted++;
        }

        if (NULL != numInserted) {
            *numInserted += keys.size();
        }

        return Status::OK();
    }

    Status BtreeBasedAccessMethod::commitBulk(OperationContext* txn,
                                              std::unique_ptr<BulkBuilder> bulk,
                                              bool mayInterrupt,
                                              bool dupsAllowed,
                                              set<RecordId>* dupsToDrop) {

        Timer timer;

        std::unique_ptr<BulkBuilder::Sorter::Iterator> i(bulk->_sorter->done());

        ProgressMeterHolder pm(*txn->setMessage("Index Bulk Build: (2/3) btree bottom up",
                                                "Index: (2/3) BTree Bottom Up Progress",
                                                bulk->_keysInserted,
                                                10));

        std::unique_ptr<SortedDataBuilderInterface> builder;

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wunit(txn);

            if (bulk->_isMultiKey) {
                _btreeState->setMultikey( txn );
            }

            builder.reset(_newInterface->getBulkBuilder(txn, dupsAllowed));
            wunit.commit();
        } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "setting index multikey flag", "");

        while (i->more()) {
            if (mayInterrupt) {
                txn->checkForInterrupt();
            }

            WriteUnitOfWork wunit(txn);
            // Improve performance in the btree-building phase by disabling rollback tracking.
            // This avoids copying all the written bytes to a buffer that is only used to roll back.
            // Note that this is safe to do, as this entire index-build-in-progress will be cleaned
            // up by the index system.
            txn->recoveryUnit()->setRollbackWritesDisabled();

            // Get the next datum and add it to the builder.
            BulkBuilder::Sorter::Data d = i->next();
            Status status = builder->addKey(d.first, d.second);

            if (!status.isOK()) {
                // Overlong key that's OK to skip?
                if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(txn)) {
                    continue;
                }

                // Check if this is a duplicate that's OK to skip
                if (status.code() == ErrorCodes::DuplicateKey) {
                    invariant(!dupsAllowed); // shouldn't be getting DupKey errors if dupsAllowed.

                    if (dupsToDrop) {
                        dupsToDrop->insert(d.second);
                        continue;
                    }
                }

                return status;
            }

            // If we're here either it's a dup and we're cool with it or the addKey went just
            // fine.
            pm.hit();
            wunit.commit();
        }

        pm.finished();

        txn->getCurOp()->setMessage("Index Bulk Build: (3/3) btree-middle",
                                     "Index: (3/3) BTree Middle Progress");

        LOG(timer.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit";

        builder->commit(mayInterrupt);
        return Status::OK();
    }

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
MONGO_CREATE_SORTER(mongo::BSONObj, mongo::RecordId, mongo::BtreeExternalSortComparison);
