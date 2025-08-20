#include <xrpl/json/json_value.h>
#include <xrpl/protocol/NFTSyntheticSerializer.h>
#include <xrpl/protocol/NFTokenID.h>
#include <xrpl/protocol/NFTokenOfferID.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/jss.h>

#include <memory>

namespace xrpl {
namespace RPC {

void
insertNFTSyntheticInJson(
    Json::Value& metadata,
    std::shared_ptr<STTx const> const& transaction,
    TxMeta const& transactionMeta)
{
    insertNFTokenID(metadata, transaction, transactionMeta);
    insertNFTokenOfferID(metadata, transaction, transactionMeta);
}

}  // namespace RPC
}  // namespace xrpl
