// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "EMBAnimNotifyState_ComboWindow.generated.h"

/**
 * Fenêtre de combo : tant que ce notify est actif, une nouvelle attaque peut être enchaînée
 * (frames 18-30 de l'attaque légère). Un input mémorisé (buffer 0,2 s) est consommé à l'ouverture.
 */
UCLASS(meta = (DisplayName = "EMB Combo Window"))
class EMBERFALL_API UEMBAnimNotifyState_ComboWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
